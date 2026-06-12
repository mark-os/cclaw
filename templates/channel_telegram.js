// Telegram channel extension for channel_runner
// Reactive handlers: onInit, onPoll, onRequest, onOutbox.
// All outbound HTTP goes through cclaw.send() — nothing here blocks.

var TG_MAX_MSG_LEN = 4096;
var config = {};
var offset = 0;
var dialogs = {}; // chat_id -> {type, data}

// Load telegram.json locale
var locale = JSON.parse(cclaw.getConfig("locale") || "{}");

function onInit() {
    config.token = cclaw.getConfig("bot_token");
    config.base = cclaw.getConfig("base_url") || "https://api.telegram.org";
    config.webhook_secret = cclaw.getConfig("webhook_secret") || "";

    if (!config.token) {
        cclaw.log("ERROR: no bot_token in channel_state");
        throw new Error("no bot_token");
    }

    // Restore offset
    var saved = cclaw.getConfig("tg_offset");
    if (saved) offset = parseInt(saved, 10) || 0;

    // webhook mode: platform POSTs to the daemon, proxied to onRequest.
    // Default: long-poll getUpdates.
    var mode = cclaw.getConfig("mode") || "poll";
    cclaw.log("telegram channel ready (mode=" + mode + ", offset=" + offset + ")");
    if (mode === "webhook") return {};
    return {poll: pollShape()};
}

function pollShape() {
    return {
        method: "GET",
        url: config.base + "/bot" + config.token + "/getUpdates?" +
             "timeout=30" + (offset > 0 ? "&offset=" + offset : "")
    };
}

// ── Long-poll completion ─────────────────────────────────────────

function onPoll(result) {
    if (result.error) {
        cclaw.log("poll error: " + result.error);
        return {poll: pollShape()};
    }
    processUpdates(result.body);
    return {poll: pollShape()};
}

// ── Webhook mode: proxied request from the daemon ────────────────

function onRequest(req) {
    // Verification is channel code, not daemon config: Telegram echoes the
    // secret we registered with setWebhook in this header.
    if (config.webhook_secret &&
        (req.headers["X-Telegram-Bot-Api-Secret-Token"] || "") !== config.webhook_secret)
        return {status: 401, body: "bad secret"};

    var update;
    try { update = JSON.parse(req.body); } catch (e) {
        return {status: 400, body: "bad json"};
    }
    if (update.message) processMessage(update.message);
    if (update.callback_query) processCallback(update.callback_query);
    return {status: 200, body: "ok"};
}

// ── Update processing (shared by poll + webhook) ─────────────────

function processUpdates(body) {
    var data;
    try { data = JSON.parse(body); } catch(e) {
        cclaw.log("parse error: " + e);
        return;
    }
    if (!data.ok || !data.result) return;

    for (var i = 0; i < data.result.length; i++) {
        var update = data.result[i];
        if (update.update_id >= offset) offset = update.update_id + 1;

        if (update.message) processMessage(update.message);
        if (update.callback_query) processCallback(update.callback_query);
    }

    // Persist offset
    if (offset > 0) cclaw.setConfig("tg_offset", "" + offset);
}

// ── Outbox delivery ──────────────────────────────────────────────

function onOutbox(item) {
    var payload;
    try { payload = JSON.parse(item.payload); } catch(e) {
        cclaw.failOutbox(item.id, "JSON parse error");
        return;
    }

    var chatId = payload.chat_id || payload.channel_id;
    var text = payload.text;
    if (!chatId || !text) {
        cclaw.failOutbox(item.id, "missing chat_id or text");
        return;
    }

    // Queue all chunks; the C loop sends them in order and acks the
    // outbox row when the final chunk gets a 2xx.
    sendChunked(chatId, text, item.id);
}

// ── Message processing ──────────────────────────────────────────

function processMessage(msg) {
    var chat = msg.chat;
    var text = msg.text;
    if (!chat || !text) return;

    var chatId = "" + chat.id;

    // Check for pending dialog reply
    if (dialogs[chatId]) {
        handleDialogReply(chatId, text);
        return;
    }

    // Admin commands
    if (text.indexOf("/key") === 0 || text.indexOf("/config") === 0 ||
        text.indexOf("/whitelist") === 0) {
        if (!cclaw.admin.isAdmin(chatId)) return;
        dispatchAdminCommand(chatId, text);
        return;
    }

    // Regular message → emit to daemon
    var payload = JSON.stringify({
        channel_id: chatId,
        text: text,
        from: msg.from ? msg.from.first_name || "" : ""
    });
    cclaw.emit("message", payload);
}

function processCallback(cbq) {
    var fromId = cbq.from ? "" + cbq.from.id : "";
    if (!cclaw.admin.isAdmin(fromId)) return;

    // Answer callback to dismiss loading
    if (cbq.id) {
        tgCall("answerCallbackQuery", JSON.stringify({callback_query_id: cbq.id}));
    }

    var data = cbq.data || "";
    if (data.indexOf("key:") === 0) handleKeyCallback(fromId, data);
    else if (data.indexOf("cfg_model:") === 0) handleConfigModelCallback(fromId, data);
    else if (data.indexOf("cfg_ep:") === 0) handleConfigEndpointCallback(fromId, data);
    else if (data.indexOf("wl:") === 0) handleWhitelistCallback(fromId, data);
}

// ── Admin command dispatch ───────────────────────────────────────

function dispatchAdminCommand(chatId, text) {
    if (text.indexOf("/key") === 0) {
        sendKeyboard(chatId, msg("key_select_provider"),
            locale.keyboards && locale.keyboards.key_providers || [
                [{"text": "OpenRouter", "callback_data": "key:openrouter"}],
                [{"text": "Gemini", "callback_data": "key:gemini"}],
                [{"text": "Custom", "callback_data": "key:custom"}]
            ]);
    } else if (text.indexOf("/config") === 0) {
        var args = text.substring(7).trim();
        if (args === "model") sendProviderKeyboard(chatId, "cfg_model");
        else if (args === "endpoint") sendProviderKeyboard(chatId, "cfg_ep");
        else sendMessage(chatId, msg("config_usage"));
    } else if (text.indexOf("/whitelist") === 0) {
        var args2 = text.substring(10).trim();
        var removing = args2 === "remove" ? 1 : 0;
        sendAgentKeyboard(chatId, removing);
    } else {
        sendMessage(chatId, msg("unknown_command"));
    }
}

// ── Key dialog ───────────────────────────────────────────────────

function handleKeyCallback(chatId, data) {
    var provider = data.substring(4);
    dialogs[chatId] = {type: "key", provider: provider};

    var prompt = provider === "custom" ? msg("key_prompt_custom") :
        msg("key_prompt_" + provider) || ("Send the API key for " + provider + ":");
    sendForceReply(chatId, prompt);
}

// ── Config model dialog ──────────────────────────────────────────

function handleConfigModelCallback(chatId, data) {
    var idx = parseInt(data.substring(10), 10);
    dialogs[chatId] = {type: "config_model", index: idx};
    sendForceReply(chatId, msg("config_enter_model"));
}

// ── Config endpoint dialog ───────────────────────────────────────

function handleConfigEndpointCallback(chatId, data) {
    var idx = parseInt(data.substring(7), 10);
    dialogs[chatId] = {type: "config_endpoint", index: idx};
    sendForceReply(chatId, msg("config_enter_endpoint"));
}

// ── Whitelist dialog ─────────────────────────────────────────────

function handleWhitelistCallback(chatId, data) {
    // Parse "wl:<agent>:<removing>"
    var parts = data.substring(3);
    var lastColon = parts.lastIndexOf(":");
    var agent = parts.substring(0, lastColon);
    var removing = parseInt(parts.substring(lastColon + 1), 10);
    dialogs[chatId] = {type: "whitelist", agent: agent, removing: removing};
    sendForceReply(chatId, removing ? msg("wl_enter_host_remove") : msg("wl_enter_host_add"));
}

// ── Dialog reply handler ─────────────────────────────────────────

function handleDialogReply(chatId, text) {
    var d = dialogs[chatId];
    delete dialogs[chatId];

    if (d.type === "key") {
        var rc = cclaw.admin.setKey(d.provider, text);
        sendMessage(chatId, rc === 0 ? msg("key_saved") : msg("key_failed"));
    } else if (d.type === "config_model") {
        var rc2 = cclaw.admin.setModel(d.index, text);
        sendMessage(chatId, rc2 === 0 ? msg("config_model_updated") + text : "Failed to update model.");
    } else if (d.type === "config_endpoint") {
        if (text.indexOf("http://") !== 0 && text.indexOf("https://") !== 0) {
            sendMessage(chatId, msg("config_endpoint_invalid"));
            return;
        }
        var rc3 = cclaw.admin.setEndpoint(d.index, text);
        sendMessage(chatId, rc3 === 0 ? msg("config_endpoint_updated") + text : "Failed to update endpoint.");
    } else if (d.type === "whitelist") {
        var rc4 = d.removing ?
            cclaw.admin.removeHost(d.agent, text) :
            cclaw.admin.addHost(d.agent, text);
        sendMessage(chatId, rc4 === 0 ?
            (d.removing ? msg("wl_host_removed") : msg("wl_host_added")) :
            msg("wl_failed"));
    }
}

// ── Telegram API helpers (queue shapes; the C loop executes) ─────

function tgCall(method, body, extra) {
    var req = {
        method: "POST",
        url: config.base + "/bot" + config.token + "/" + method,
        body: body
    };
    if (extra) {
        if (extra.outbox_id) req.outbox_id = extra.outbox_id;
        if (extra.final) req.final = 1;
    }
    cclaw.send(req);
}

function sendMessage(chatId, text) {
    tgCall("sendMessage", JSON.stringify({chat_id: parseInt(chatId, 10), text: text}));
}

function sendChunked(chatId, text, outboxId) {
    var total = text.length;
    var pos = 0;
    while (pos < total) {
        var remaining = total - pos;
        var chunkLen = remaining <= TG_MAX_MSG_LEN ? remaining : findSplit(text, pos, TG_MAX_MSG_LEN);
        var chunk = text.substring(pos, pos + chunkLen);
        pos += chunkLen;
        tgCall("sendMessage",
            JSON.stringify({chat_id: parseInt(chatId, 10), text: chunk}),
            {outbox_id: outboxId, final: pos >= total});
    }
}

function findSplit(text, start, maxLen) {
    var end = start + maxLen;
    if (end >= text.length) return text.length - start;
    // Try double newline
    for (var i = end; i > start; i--) {
        if (text[i] === "\n" && i > start && text[i-1] === "\n") return i - start + 1;
    }
    // Try single newline
    for (var i2 = end; i2 > start; i2--) {
        if (text[i2] === "\n") return i2 - start + 1;
    }
    // Try sentence end
    for (var i3 = end; i3 > start; i3--) {
        var c = text[i3 - 1];
        if ((c === "." || c === "!" || c === "?") &&
            (i3 === text.length || text[i3] === " " || text[i3] === "\n"))
            return i3 - start;
    }
    return maxLen;
}

function sendKeyboard(chatId, text, keyboard) {
    tgCall("sendMessage", JSON.stringify({
        chat_id: parseInt(chatId, 10),
        text: text,
        reply_markup: {inline_keyboard: keyboard}
    }));
}

function sendForceReply(chatId, text) {
    tgCall("sendMessage", JSON.stringify({
        chat_id: parseInt(chatId, 10),
        text: text,
        reply_markup: {force_reply: true, selective: true}
    }));
}

function sendProviderKeyboard(chatId, prefix) {
    var providers = cclaw.admin.listProviders();
    var keyboard = [];
    for (var i = 0; i < providers.length; i++) {
        var p = providers[i];
        var label = (i === 0 ? "Primary: " : "Fallback " + i + ": ") + (p.model || "(none)");
        keyboard.push([{text: label, callback_data: prefix + ":" + i}]);
    }
    var title = prefix === "cfg_model" ? msg("config_select_provider") : msg("config_select_endpoint");
    sendKeyboard(chatId, title, keyboard);
}

function sendAgentKeyboard(chatId, removing) {
    var agents = cclaw.admin.listAgents();
    if (!agents || agents.length === 0) {
        sendMessage(chatId, "No agents found.");
        return;
    }
    var keyboard = [];
    for (var i = 0; i < agents.length && i < 10; i++) {
        keyboard.push([{text: agents[i], callback_data: "wl:" + agents[i] + ":" + removing}]);
    }
    sendKeyboard(chatId, msg("wl_select_agent"), keyboard);
}

// ── Helpers ──────────────────────────────────────────────────────

function msg(key) {
    return (locale.messages && locale.messages[key]) || key;
}
