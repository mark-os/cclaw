// whatsapp-bridge — reference bridge for cclaw's whatsapp channel.
// One WhatsApp account in, the HTTP API of specs/whatsapp.md out.
//
// Config is environment only:
//   WAB_LISTEN  bind address           (default 127.0.0.1:8471)
//   WAB_TOKEN   API token              (default none — loopback only!)
//   WAB_DB      session store path     (default whatsapp.db)
//   WAB_PHONE   pair-code number       (default none — QR to log instead)
#![recursion_limit = "512"]

use log::{error, info, warn};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex, OnceLock};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::Notify;

use serde_json::{Value, json};
use whatsapp_rust::pair_code::PairCodeOptions;
use whatsapp_rust::prelude::*;

const EVENT_BUF_MAX: usize = 512;
const POLL_TIMEOUT_MAX: u64 = 30;
const POLL_TIMEOUT_DEFAULT: u64 = 25;
const HTTP_REQ_MAX: usize = 64 * 1024;

struct State {
    // (id, event-json) — ids are per-process; the buffer is memory, not a
    // journal (specs/whatsapp.md: WhatsApp's own offline queue makes the
    // end-to-end path lossless across bridge restarts).
    events: Mutex<(VecDeque<(u64, Value)>, u64)>, // (buf, next_id)
    notify: Notify,
    status: Mutex<Value>,
    client: OnceLock<Arc<whatsapp_rust::client::Client>>,
    token: Option<String>,
    phone: Option<String>,
}

impl State {
    fn push_event(&self, mut ev: Value) {
        let mut g = self.events.lock().unwrap();
        let id = g.1;
        g.1 += 1;
        ev["id"] = json!(id);
        g.0.push_back((id, ev));
        while g.0.len() > EVENT_BUF_MAX {
            g.0.pop_front();
        }
        drop(g);
        self.notify.notify_waiters();
    }

    fn set_state(&self, state: &str, extra: &[(&str, Value)]) {
        let mut ev = json!({"type": "status", "state": state});
        let mut st = json!({"state": state, "bridge": "whatsapp-bridge",
                            "version": env!("CARGO_PKG_VERSION"), "api": 1});
        for (k, v) in extra {
            ev[*k] = v.clone();
            st[*k] = v.clone();
        }
        *self.status.lock().unwrap() = st;
        self.push_event(ev);
    }
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let listen = std::env::var("WAB_LISTEN").unwrap_or_else(|_| "127.0.0.1:8471".into());
    let token = std::env::var("WAB_TOKEN").ok().filter(|t| !t.is_empty());
    let db = std::env::var("WAB_DB").unwrap_or_else(|_| "whatsapp.db".into());
    let phone = std::env::var("WAB_PHONE").ok().filter(|p| !p.is_empty());

    if token.is_none() && !listen.starts_with("127.") && !listen.starts_with("localhost") {
        warn!("WAB_LISTEN={} is not loopback and WAB_TOKEN is unset — anyone who can reach the port owns the account", listen);
    }

    let state = Arc::new(State {
        events: Mutex::new((VecDeque::new(), 1)),
        notify: Notify::new(),
        status: Mutex::new(json!({"state": "starting"})),
        client: OnceLock::new(),
        token,
        phone: phone.clone(),
    });
    state.set_state("starting", &[]);

    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("tokio runtime");

    rt.block_on(async {
        let listener = TcpListener::bind(&listen).await.unwrap_or_else(|e| {
            error!("bind {listen}: {e}");
            std::process::exit(1);
        });
        info!("whatsapp-bridge listening on {listen}");
        tokio::spawn(serve(state.clone(), listener));

        let store = match SqliteStore::new(&db).await {
            Ok(s) => s,
            Err(e) => {
                error!("session store {db}: {e}");
                std::process::exit(1);
            }
        };

        let st = state.clone();
        let st_qr = state.clone();
        let st_pair = state.clone();
        let st_conn = state.clone();
        let st_out = state.clone();
        let mut builder = Bot::builder()
            .with_backend(store)
            .on_qr_code(move |code, timeout| {
                let st = st_qr.clone();
                async move {
                    info!("QR code (valid {}s):\n{}", timeout.as_secs(), code);
                    st.set_state("pairing", &[("qr", json!(code))]);
                }
            })
            .on_pair_code(move |code, timeout| {
                let st = st_pair.clone();
                async move {
                    info!("PAIR CODE (valid {}s): {}", timeout.as_secs(), code);
                    info!("WhatsApp > Linked Devices > Link a Device > Link with phone number instead");
                    st.set_state("pairing", &[("pair_code", json!(code))]);
                }
            })
            .on_connected(move |client| {
                let st = st_conn.clone();
                async move {
                    info!("connected");
                    let _ = st.client.set(client);
                    st.set_state("connected", &[]);
                }
            })
            .on_logged_out(move |_info| {
                let st = st_out.clone();
                async move {
                    error!("logged out — re-pairing required");
                    st.set_state("logged_out", &[]);
                }
            })
            .on_message(move |ctx| {
                let st = st.clone();
                async move { handle_message(&st, &ctx) }
            });

        if let Some(p) = phone {
            builder = builder.with_pair_code(PairCodeOptions {
                phone_number: p,
                ..Default::default()
            });
        }

        let bot = match builder.build().await {
            Ok(b) => b,
            Err(e) => {
                error!("bot build: {e}");
                std::process::exit(1);
            }
        };
        bot.run().await;
    });
}

// ── WhatsApp → event buffer ─────────────────────────────────────

fn handle_message(st: &State, ctx: &MessageContext) {
    let src = &ctx.info.source;
    if src.is_from_me {
        return; // our own sends echo back; forwarding them would loop
    }
    let base = ctx.message.get_base_message();
    let text = ctx
        .message
        .text_content()
        .or_else(|| ctx.message.get_caption())
        .unwrap_or("");
    if text.is_empty() {
        return; // text-only v1
    }

    // mentioned / reply_to_me: compare against the configured own number
    // (no public own-jid accessor on Client; WAB_PHONE is authoritative
    // enough for an advisory fact — absent it, both report false).
    let (mut mentioned, mut reply_to_me) = (false, false);
    if let Some(phone) = &st.phone
        && let Some(ext) = base.extended_text_message.as_option()
        && let Some(ci) = ext.context_info.as_option()
    {
        mentioned = ci
            .mentioned_jid
            .iter()
            .any(|j| j.split('@').next() == Some(phone.as_str()));
        reply_to_me = ci
            .participant
            .as_deref()
            .is_some_and(|p| p.split('@').next() == Some(phone.as_str()));
    }

    st.push_event(json!({
        "type": "message",
        "msg_id": ctx.info.id,
        "chat_id": src.chat.to_string(),
        "sender_id": src.sender.to_non_ad().to_string(),
        "sender_name": ctx.info.push_name,
        "chat_type": if src.is_group { "group" } else { "dm" },
        "text": text,
        "mentioned": mentioned,
        "reply_to_me": reply_to_me,
        "ts": ctx.info.timestamp.timestamp(),
    }));
}

// ── HTTP API (hand-rolled: one request per connection) ──────────

async fn serve(state: Arc<State>, listener: TcpListener) {
    loop {
        match listener.accept().await {
            Ok((sock, _)) => {
                let st = state.clone();
                tokio::spawn(async move {
                    if let Err(e) = handle_conn(st, sock).await {
                        log::debug!("http conn: {e}");
                    }
                });
            }
            Err(e) => {
                error!("accept: {e}");
                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            }
        }
    }
}

async fn handle_conn(st: Arc<State>, mut sock: TcpStream) -> std::io::Result<()> {
    let mut buf = Vec::with_capacity(2048);
    let mut tmp = [0u8; 2048];
    let header_end = loop {
        let n = sock.read(&mut tmp).await?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
        if let Some(pos) = find_subslice(&buf, b"\r\n\r\n") {
            break pos + 4;
        }
        if buf.len() > HTTP_REQ_MAX {
            return respond(&mut sock, 431, &json!({"error": "headers too large"})).await;
        }
    };

    let head = String::from_utf8_lossy(&buf[..header_end]).into_owned();
    let mut lines = head.split("\r\n");
    let reqline = lines.next().unwrap_or("");
    let mut parts = reqline.split(' ');
    let method = parts.next().unwrap_or("").to_string();
    let target = parts.next().unwrap_or("").to_string();
    let (path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), q.to_string()),
        None => (target.clone(), String::new()),
    };

    let mut content_length = 0usize;
    let mut auth = String::new();
    for l in lines {
        let Some((k, v)) = l.split_once(':') else { continue };
        let v = v.trim();
        if k.eq_ignore_ascii_case("content-length") {
            content_length = v.parse().unwrap_or(0);
        } else if k.eq_ignore_ascii_case("authorization") {
            auth = v.to_string();
        }
    }
    if content_length > HTTP_REQ_MAX {
        return respond(&mut sock, 413, &json!({"error": "body too large"})).await;
    }
    let mut body = buf[header_end..].to_vec();
    while body.len() < content_length {
        let n = sock.read(&mut tmp).await?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }

    // Auth: Bearer header or ?token= (the runner's poll shape has no headers).
    if let Some(tok) = &st.token {
        let bearer_ok = auth.strip_prefix("Bearer ").map(str::trim) == Some(tok.as_str());
        let query_ok = query_param(&query, "token").as_deref() == Some(tok.as_str());
        if !bearer_ok && !query_ok {
            return respond(&mut sock, 401, &json!({"error": "bad token"})).await;
        }
    }

    match (method.as_str(), path.as_str()) {
        ("GET", "/v1/status") => {
            let s = st.status.lock().unwrap().clone();
            respond(&mut sock, 200, &s).await
        }
        ("GET", "/v1/events") => {
            let cursor: u64 = query_param(&query, "cursor")
                .and_then(|c| c.parse().ok())
                .unwrap_or(0);
            let timeout = query_param(&query, "timeout")
                .and_then(|t| t.parse().ok())
                .unwrap_or(POLL_TIMEOUT_DEFAULT)
                .min(POLL_TIMEOUT_MAX);
            let out = poll_events(&st, cursor, timeout).await;
            respond(&mut sock, 200, &out).await
        }
        ("POST", "/v1/send") => {
            let (code, out) = do_send(&st, &body).await;
            respond(&mut sock, code, &out).await
        }
        ("POST", "/v1/pair") => {
            respond(&mut sock, 501, &json!({"error": "pairing is configured via WAB_PHONE"})).await
        }
        _ => respond(&mut sock, 404, &json!({"error": "not found"})).await,
    }
}

async fn poll_events(st: &State, cursor: u64, timeout_s: u64) -> Value {
    let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(timeout_s);
    loop {
        {
            let g = st.events.lock().unwrap();
            let tail = g.1 - 1; // highest id ever assigned (0 = none yet)
            // A cursor from a previous incarnation (ahead of our tail):
            // answer immediately with the current cursor, no events — the
            // client adopts it and resynchronizes.
            if cursor > tail {
                return json!({"cursor": tail, "events": []});
            }
            let evs: Vec<Value> = g
                .0
                .iter()
                .filter(|(id, _)| *id > cursor)
                .map(|(_, e)| e.clone())
                .collect();
            if !evs.is_empty() {
                return json!({"cursor": tail, "events": evs});
            }
        }
        let notified = st.notify.notified();
        if tokio::time::timeout_at(deadline, notified).await.is_err() {
            let tail = st.events.lock().unwrap().1 - 1;
            return json!({"cursor": if cursor < tail { cursor } else { tail }, "events": []});
        }
    }
}

async fn do_send(st: &State, body: &[u8]) -> (u16, Value) {
    let Ok(req) = serde_json::from_slice::<Value>(body) else {
        return (400, json!({"error": "bad json"}));
    };
    let (Some(chat_id), Some(text)) = (req["chat_id"].as_str(), req["text"].as_str()) else {
        return (400, json!({"error": "missing chat_id or text"}));
    };
    let jid: Jid = match chat_id.parse() {
        Ok(j) => j,
        Err(e) => return (400, json!({"error": format!("bad jid: {e}")})),
    };
    let Some(client) = st.client.get() else {
        return (502, json!({"error": "not connected"}));
    };
    match client.send_message(jid, wa::Message::text(text)).await {
        Ok(sent) => (200, json!({"ok": true, "message_id": sent.message_id})),
        Err(e) => (502, json!({"error": format!("send failed: {e}")})),
    }
}

async fn respond(sock: &mut TcpStream, code: u16, body: &Value) -> std::io::Result<()> {
    let b = serde_json::to_string(body).unwrap_or_else(|_| "{}".into());
    let reason = match code {
        200 => "OK",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        413 => "Payload Too Large",
        431 => "Request Header Fields Too Large",
        501 => "Not Implemented",
        _ => "Error",
    };
    let resp = format!(
        "HTTP/1.1 {code} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{b}",
        b.len()
    );
    sock.write_all(resp.as_bytes()).await?;
    sock.shutdown().await
}

fn find_subslice(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn query_param(query: &str, key: &str) -> Option<String> {
    query.split('&').find_map(|kv| {
        let (k, v) = kv.split_once('=')?;
        (k == key).then(|| pct_decode(v))
    })
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 3 <= b.len() {
            if let Ok(h) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(h);
                i += 3;
                continue;
            }
        }
        out.push(if b[i] == b'+' { b' ' } else { b[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}
