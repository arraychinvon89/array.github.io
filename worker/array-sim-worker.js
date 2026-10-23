// =============================================================================
//  array_simulator server — Cloudflare Worker (single file, no build step)
//
//  What it does
//    GET  /me        which plan the signed-in visitor has (Free / Pro / Ultimate) and AI messages left today
//    POST /checkout  opens a Stripe Checkout page for Pro (99 THB / month) or Ultimate (599 THB / year)
//    POST /portal    opens the Stripe customer portal (change card, cancel)
//    POST /webhook   Stripe tells us a payment went through / a subscription changed
//    POST /ai        the AI assistant for Pro and Ultimate members, using the site owner's Anthropic key
//
//  Every request from the website carries the visitor's Firebase ID token (Google sign-in);
//  the worker checks its signature against Google's public keys, so nobody can pretend to be someone else.
//
//  Settings (Cloudflare dashboard → Workers → this worker → Settings → Variables and Secrets)
//    secrets : ANTHROPIC_API_KEY, STRIPE_SECRET_KEY, STRIPE_WEBHOOK_SECRET
//    text    : FIREBASE_PROJECT_ID = array-simulator
//              SITE_URL            = https://arraychinvon89.github.io/array.github.io/
//              OWNER_EMAIL         = the owner's Google e-mail (always Ultimate, never charged)
//    optional: AI_MODEL (default claude-haiku-5-5), AI_DAILY_PRO (default 20), AI_DAILY_ULTIMATE (default 30)
//    KV namespace binding: DB
// =============================================================================

const PLANS = {
  pro:      { name: 'array_simulator Pro',      amount: 9900,  interval: 'month', days: 31 },
  ultimate: { name: 'array_simulator Ultimate', amount: 59900, interval: 'year',  days: 366 },
};
const RANK = { free: 1, pro: 2, ultimate: 3 };

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const cors = corsHeaders(req, env);
    if (req.method === 'OPTIONS') return new Response(null, { status: 204, headers: cors });
    try {
      if (url.pathname === '/webhook' && req.method === 'POST') return await webhook(req, env);
      if (url.pathname === '/' || url.pathname === '/health') return json({ ok: true, service: 'array_simulator' }, 200, cors);
      const user = await authUser(req, env);
      if (!user) return json({ error: 'sign in required' }, 401, cors);
      if (url.pathname === '/me' && req.method === 'GET') return json(await me(user, env), 200, cors);
      if (url.pathname === '/checkout' && req.method === 'POST') return json(await checkout(user, await req.json(), env), 200, cors);
      if (url.pathname === '/portal' && req.method === 'POST') return json(await portal(user, env), 200, cors);
      if (url.pathname === '/ai' && req.method === 'POST') return await ai(user, req, env, cors);
      return json({ error: 'not found' }, 404, cors);
    } catch (e) {
      const status = e.status || 500;
      if (status >= 500) console.error(e.stack || e);
      return json({ error: e.expose ? e.message : status >= 500 ? 'server error' : e.message }, status, cors);
    }
  },
};

// ----------------------------------------------------------------------------- helpers
function json(data, status = 200, headers = {}) {
  return new Response(JSON.stringify(data), { status, headers: { 'content-type': 'application/json', ...headers } });
}
function fail(status, message) { const e = new Error(message); e.status = status; e.expose = true; return e; }
function corsHeaders(req, env) {
  const origin = req.headers.get('origin') || '';
  const site = env.SITE_URL ? new URL(env.SITE_URL).origin : '';
  const ok = origin === site || /^https?:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/.test(origin);
  return ok ? { 'access-control-allow-origin': origin, 'access-control-allow-headers': 'authorization, content-type, x-ai-ticket',
    'access-control-allow-methods': 'GET, POST, OPTIONS', 'access-control-expose-headers': 'x-ai-left, x-ai-ticket', 'access-control-max-age': '86400', vary: 'origin' } : { vary: 'origin' };
}
const b64urlBytes = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/') + '==='.slice((s.length + 3) % 4)), c => c.charCodeAt(0));
const b64urlJson = s => JSON.parse(new TextDecoder().decode(b64urlBytes(s)));
const today = () => new Date(Date.now() + 7 * 3600e3).toISOString().slice(0, 10);   // the day in Thailand (UTC+7)

// ----------------------------------------------------------------------------- Firebase ID token check
let jwks = null, jwksUntil = 0;
async function googleKeys() {
  if (jwks && Date.now() < jwksUntil) return jwks;
  const res = await fetch('https://www.googleapis.com/service_accounts/v1/jwk/securetoken@system.gserviceaccount.com');
  if (!res.ok) throw new Error('cannot load Google keys');
  const maxAge = +((res.headers.get('cache-control') || '').match(/max-age=(\d+)/) || [0, 3600])[1];
  jwks = (await res.json()).keys; jwksUntil = Date.now() + maxAge * 1000;
  return jwks;
}
async function authUser(req, env) {
  const m = (req.headers.get('authorization') || '').match(/^Bearer (.+)$/);
  if (!m) return null;
  const parts = m[1].split('.');
  if (parts.length !== 3) return null;
  let head, body;
  try { head = b64urlJson(parts[0]); body = b64urlJson(parts[1]); } catch (e) { return null; }
  if (head.alg !== 'RS256') return null;
  const jwk = (await googleKeys()).find(k => k.kid === head.kid);
  if (!jwk) return null;
  const key = await crypto.subtle.importKey('jwk', jwk, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['verify']);
  const ok = await crypto.subtle.verify('RSASSA-PKCS1-v1_5', key, b64urlBytes(parts[2]), new TextEncoder().encode(parts[0] + '.' + parts[1]));
  const now = Date.now() / 1000, pid = env.FIREBASE_PROJECT_ID;
  if (!ok || body.aud !== pid || body.iss !== 'https://securetoken.google.com/' + pid || !(body.exp > now) || !(body.iat <= now + 60) || !body.sub) return null;
  return { uid: body.sub, email: body.email || '', verified: !!body.email_verified };
}

// ----------------------------------------------------------------------------- plan bookkeeping (KV)
// user:<uid> -> { pro: <ms>, ultimate: <ms>, customer, sub: { id, plan }, email }
async function getRec(env, uid) { return (await env.DB.get('user:' + uid, 'json')) || {}; }
async function putRec(env, uid, rec) { await env.DB.put('user:' + uid, JSON.stringify(rec)); }
function isOwner(user, env) { return !!env.OWNER_EMAIL && user.verified && user.email.toLowerCase() === env.OWNER_EMAIL.toLowerCase(); }
function tierOf(rec, user, env) {
  if (user && isOwner(user, env)) return { tier: 'ultimate', until: 0 };
  const now = Date.now();
  if ((rec.ultimate || 0) > now) return { tier: 'ultimate', until: rec.ultimate };
  if ((rec.pro || 0) > now) return { tier: 'pro', until: rec.pro };
  return { tier: 'free', until: 0 };
}
const aiLimit = (tier, env) => tier === 'ultimate' ? +(env.AI_DAILY_ULTIMATE || 30) : tier === 'pro' ? +(env.AI_DAILY_PRO || 20) : 0;
async function me(user, env) {
  const rec = await getRec(env, user.uid), t = tierOf(rec, user, env);
  const used = +(await env.DB.get('ai:' + user.uid + ':' + today()) || 0), lim = aiLimit(t.tier, env);
  return { tier: t.tier, until: t.until, sub: !!(rec.sub && rec.sub.id && t.tier !== 'free'), aiLeft: lim ? Math.max(0, lim - used) : null, aiLimit: lim || null };
}

// ----------------------------------------------------------------------------- Stripe
async function stripe(env, path, params, method = 'POST') {
  const body = params ? new URLSearchParams(flat(params)).toString() : undefined;
  const res = await fetch('https://api.stripe.com/v1/' + path + (method === 'GET' && body ? '?' + body : ''), {
    method, headers: { authorization: 'Bearer ' + env.STRIPE_SECRET_KEY, 'content-type': 'application/x-www-form-urlencoded' }, body: method === 'GET' ? undefined : body });
  const data = await res.json();
  if (!res.ok) { console.error('stripe', path, data.error && data.error.message); throw fail(502, 'Stripe: ' + ((data.error && data.error.message) || res.status)); }
  return data;
}
function flat(obj, pre = '', out = {}) {     // { a: { b: 1 }, c: [x] } -> { 'a[b]': 1, 'c[0]': x }
  for (const [k, v] of Object.entries(obj)) {
    const key = pre ? pre + '[' + k + ']' : k;
    if (v === undefined || v === null) continue;
    if (typeof v === 'object') flat(v, key, out); else out[key] = String(v);
  }
  return out;
}
async function customerFor(user, rec, env) {
  if (rec.customer) return rec.customer;
  const c = await stripe(env, 'customers', { email: user.email || undefined, metadata: { uid: user.uid } });
  rec.customer = c.id; rec.email = user.email;
  await putRec(env, user.uid, rec); await env.DB.put('cust:' + c.id, user.uid);
  return c.id;
}
async function checkout(user, body, env) {
  const plan = PLANS[body.plan] ? body.plan : null;
  if (!plan) throw fail(400, 'unknown plan');
  const method = body.method === 'promptpay' ? 'promptpay' : 'card';
  const rec = await getRec(env, user.uid);
  // an active card subscription can only be upgraded (Pro -> Ultimate); the old one is then stopped by the webhook
  if (method === 'card' && rec.sub && rec.sub.id && tierOf(rec, user, env).tier !== 'free' && RANK[plan] <= RANK[rec.sub.plan]) throw fail(409, 'มีการสมัครแบบต่ออายุอัตโนมัติอยู่แล้ว จัดการได้ที่ปุ่มจัดการการสมัคร');
  const customer = await customerFor(user, rec, env);
  const P = PLANS[plan], site = env.SITE_URL;
  const price = { currency: 'thb', unit_amount: P.amount, product_data: { name: P.name } };
  const common = { customer, client_reference_id: user.uid, metadata: { uid: user.uid, plan }, locale: 'th',
    success_url: site + '?paid=' + plan, cancel_url: site + '?paid=cancel' };
  const params = method === 'card'
    ? { ...common, mode: 'subscription', payment_method_types: ['card'], line_items: [{ quantity: 1, price_data: { ...price, recurring: { interval: P.interval } } }],
        subscription_data: { metadata: { uid: user.uid, plan } } }
    : { ...common, mode: 'payment', payment_method_types: ['promptpay'], line_items: [{ quantity: 1, price_data: price }],
        payment_intent_data: { metadata: { uid: user.uid, plan } } };
  const session = await stripe(env, 'checkout/sessions', params);
  return { url: session.url };
}
async function portal(user, env) {
  const rec = await getRec(env, user.uid);
  if (!rec.customer) throw fail(404, 'ยังไม่เคยสมัครสมาชิก');
  const s = await stripe(env, 'billing_portal/sessions', { customer: rec.customer, return_url: env.SITE_URL });
  return { url: s.url };
}

async function verifyStripe(req, env, raw) {
  const sig = req.headers.get('stripe-signature') || '';
  const t = (sig.match(/t=(\d+)/) || [])[1], v1s = [...sig.matchAll(/v1=([0-9a-f]+)/g)].map(m => m[1]);
  if (!t || !v1s.length || Math.abs(Date.now() / 1000 - +t) > 300) return false;
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(env.STRIPE_WEBHOOK_SECRET), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(t + '.' + raw)));
  const hex = [...mac].map(b => b.toString(16).padStart(2, '0')).join('');
  return v1s.some(v => v.length === hex.length && [...v].reduce((d, c, i) => d | (c.charCodeAt(0) ^ hex.charCodeAt(i)), 0) === 0);
}
const periodEnd = sub => 1000 * (sub.current_period_end || (sub.items && sub.items.data && sub.items.data[0] && sub.items.data[0].current_period_end) || 0);
async function applySubscription(env, sub) {
  const uid = (sub.metadata && sub.metadata.uid) || await env.DB.get('cust:' + sub.customer);
  const plan = sub.metadata && sub.metadata.plan;
  if (!uid || !PLANS[plan]) return;
  const rec = await getRec(env, uid);
  const active = ['active', 'trialing', 'past_due'].includes(sub.status);
  // paid-through date plus a day of grace for the renewal charge to settle
  const end = active ? periodEnd(sub) + 864e5 : Math.min(Date.now(), periodEnd(sub) || Date.now());
  const prepaid = (rec.oneoff && rec.oneoff[plan]) || 0;            // PromptPay time bought separately is kept
  rec[plan] = Math.max(end, prepaid);
  rec.customer = rec.customer || sub.customer;
  const prev = rec.sub;
  if (active && !sub.cancel_at_period_end) {
    // upgraded Pro -> Ultimate: stop renewing the old Pro subscription (it runs out at the end of the month already paid)
    if (prev && prev.id && prev.id !== sub.id && RANK[plan] > RANK[prev.plan]) {
      await stripe(env, 'subscriptions/' + prev.id, { cancel_at_period_end: true }).catch(e => console.error('cancel old sub', e.message));
    }
    if (!prev || prev.id === sub.id || RANK[plan] >= RANK[prev.plan]) rec.sub = { id: sub.id, plan };
  } else if (prev && prev.id === sub.id) {
    rec.sub = active ? { id: sub.id, plan, ending: true } : null;
  }
  await putRec(env, uid, rec);
  if (sub.customer) await env.DB.put('cust:' + sub.customer, uid);
}
async function grantOneOff(env, uid, plan, ref) {
  if (!uid || !PLANS[plan]) return;
  if (await env.DB.get('paid:' + ref)) return;                 // Stripe may send the same event twice
  const rec = await getRec(env, uid);
  const from = Math.max(Date.now(), rec[plan] || 0);
  rec[plan] = from + PLANS[plan].days * 864e5;
  rec.oneoff = Object.assign({}, rec.oneoff, { [plan]: rec[plan] });
  await putRec(env, uid, rec);
  await env.DB.put('paid:' + ref, '1', { expirationTtl: 60 * 60 * 24 * 90 });
}
async function webhook(req, env) {
  const raw = await req.text();
  if (!(await verifyStripe(req, env, raw))) return new Response('bad signature', { status: 400 });
  const ev = JSON.parse(raw), o = ev.data.object;
  switch (ev.type) {
    case 'checkout.session.completed':
    case 'checkout.session.async_payment_succeeded': {
      const uid = o.client_reference_id || (o.metadata && o.metadata.uid), plan = o.metadata && o.metadata.plan;
      if (o.customer && uid) await env.DB.put('cust:' + o.customer, uid);
      if (o.mode === 'subscription' && o.subscription) await applySubscription(env, await stripe(env, 'subscriptions/' + o.subscription, null, 'GET'));
      else if (o.mode === 'payment' && o.payment_status === 'paid') await grantOneOff(env, uid, plan, o.id);
      break;
    }
    case 'customer.subscription.created':
    case 'customer.subscription.updated':
    case 'customer.subscription.deleted':
      await applySubscription(env, o); break;
    case 'invoice.paid': {
      const subId = o.subscription || (o.parent && o.parent.subscription_details && o.parent.subscription_details.subscription);
      if (subId) await applySubscription(env, await stripe(env, 'subscriptions/' + subId, null, 'GET'));
      break;
    }
  }
  return new Response('ok');
}

// ----------------------------------------------------------------------------- AI assistant for members
async function ai(user, req, env, cors) {
  const rec = await getRec(env, user.uid), { tier } = tierOf(rec, user, env);
  const lim = aiLimit(tier, env);
  if (!lim) throw fail(402, 'members only');
  const raw = await req.text();
  if (raw.length > 120000) throw fail(413, 'too large');
  const body = JSON.parse(raw);
  // a question can take a few tool rounds; only the first counts towards the daily allowance.
  // Follow-up rounds carry a short-lived signed ticket from the previous answer (at most 6 rounds, 3 minutes).
  const key = 'ai:' + user.uid + ':' + today(), used = +(await env.DB.get(key) || 0);
  const tk = await readTicket(req.headers.get('x-ai-ticket'), user.uid, env);
  const first = tk === null;
  if (first && used >= lim) return json({ error: 'daily limit' }, 429, { ...cors, 'x-ai-left': '0' });
  const messages = Array.isArray(body.messages) ? body.messages.slice(-30) : [];
  const tools = Array.isArray(body.tools) ? body.tools.slice(0, 8) : undefined;
  if (tools && tools.length) tools[tools.length - 1] = { ...tools[tools.length - 1], cache_control: { type: 'ephemeral' } };
  const res = await fetch('https://api.anthropic.com/v1/messages', {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-api-key': env.ANTHROPIC_API_KEY, 'anthropic-version': '2023-06-01' },
    body: JSON.stringify({
      model: env.AI_MODEL || 'claude-haiku-5-5', max_tokens: 2048,
      system: [{ type: 'text', text: String(body.system || '').slice(0, 30000), cache_control: { type: 'ephemeral' } }],
      tools, messages, metadata: { user_id: user.uid } }),
  });
  const out = await res.text();
  if (!res.ok) { console.error('anthropic', res.status, out.slice(0, 300)); return json({ error: 'ai upstream ' + res.status }, 502, cors); }
  const left = first ? lim - used - 1 : lim - used;
  if (first) await env.DB.put(key, String(used + 1), { expirationTtl: 60 * 60 * 48 });
  const next = (first ? 0 : tk) + 1;
  const ticket = next < 6 ? await makeTicket(user.uid, next, env) : '';
  return new Response(out, { status: 200, headers: { 'content-type': 'application/json', ...cors, 'x-ai-left': String(Math.max(0, left)), 'x-ai-ticket': ticket } });
}
async function ticketMac(msg, env) {
  const key = await crypto.subtle.importKey('raw', new TextEncoder().encode('ai-ticket:' + env.ANTHROPIC_API_KEY), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return [...new Uint8Array(await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(msg)))].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 32);
}
async function makeTicket(uid, n, env) { const exp = Date.now() + 180e3, msg = uid + '.' + exp + '.' + n; return exp + '.' + n + '.' + await ticketMac(msg, env); }
async function readTicket(t, uid, env) {               // -> round number, or null when there is no valid ticket
  const m = (t || '').match(/^(\d+)\.(\d)\.([0-9a-f]{32})$/);
  if (!m || +m[1] < Date.now() || +m[2] >= 6) return null;
  return (await ticketMac(uid + '.' + m[1] + '.' + m[2], env)) === m[3] ? +m[2] : null;
}
