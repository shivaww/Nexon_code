// relay.js — injected into every chatbot page by nexon_relay.
//
// Responsibilities:
//   1. Find fenced ```json code blocks inside assistant messages.
//   2. Extract a balanced JSON value from each (mirrors nexon_code's
//      JsonAccumulator: brace/string-aware depth counter).
//   3. Shape-validate: {t:...}, {calls:[...]}, or bare [{t:...},...].
//   4. Hand qualifying blocks to NexonRelay.run() exactly once per message.
//
// This file never types anything. Input is the native side's job.
(function () {
  "use strict";
  if (window.__nexonRelayInstalled) return;
  window.__nexonRelayInstalled = true;

  var CONFIG = {
    maxIterations: 20,
    streamQuietMs: 900
  };

  // Per-site config is injected by the native side as window.__nexonSites.
  // The fallback below is deliberately broad — it catches the common shape
  // across all five providers if the site-specific selectors miss.
  var SITE = (window.__nexonSites && window.__nexonSites[0]) || {
    assistantSelector: "[data-message-author-role='assistant'], div[data-testid='assistant-message'], model-response, div[class*='assistant']",
    composerSelectors: ["div[contenteditable='true']", "textarea"],
    sendSelectors: ["button[type='submit']", "button[aria-label*='Send']"],
    stopSelectors: ["button[aria-label*='Stop']"]
  };

  function log(m) { try { console.log("[relay] " + m); } catch (e) {} }

  // ---- balanced scanner ---------------------------------------------------
  // Faithful port of nexon_code.cpp JsonAccumulator::feed(). Starts on the
  // first { or [ seen, ends when depth returns to 0 outside a string.
  // Returns the exact source slice, or null if none was found.
  function firstBalancedJson(text) {
    var start = -1, depth = 0, inString = false, escaped = false;
    for (var i = 0; i < text.length; i++) {
      var c = text[i];
      if (start < 0) {
        if (c === "{" || c === "[") { start = i; depth = 1; }
        continue;
      }
      if (inString) {
        if (escaped) escaped = false;
        else if (c === "\\") escaped = true;
        else if (c === "\"") inString = false;
        continue;
      }
      if (c === "\"") inString = true;
      else if (c === "{" || c === "[") depth++;
      else if (c === "}" || c === "]") {
        depth--;
        if (depth === 0) return { json: text.slice(start, i + 1), start: start, end: i + 1 };
      }
    }
    return null;
  }

  // ---- fenced block extraction -------------------------------------------
  function extractFencedJson(text) {
    var out = [];
    // ```json ... ``` — case-insensitive language tag, optional trailing spaces.
    var re = /```[ \t]*json[ \t]*\r?\n([\s\S]*?)```/gi;
    var m;
    while ((m = re.exec(text)) !== null) {
      var bal = firstBalancedJson(m[1]);
      if (bal) out.push(bal.json);
    }
    return out;
  }

  // ---- shape filter -------------------------------------------------------
  function looksLikeCall(obj) {
    if (!obj || typeof obj !== "object" || Array.isArray(obj)) return false;
    var t = obj.t || obj.tool;
    if (typeof t === "string" && t.length > 0) return true;
    if (Array.isArray(obj.calls) && obj.calls.length > 0) return true;
    return false;
  }

  function isRelayCommand(jsonText) {
    var v;
    try { v = JSON.parse(jsonText); } catch (e) { return false; }
    if (Array.isArray(v)) {
      if (v.length === 0) return false;
      for (var i = 0; i < v.length; i++) if (!looksLikeCall(v[i])) return false;
      return true;
    }
    return looksLikeCall(v);
  }

  // ---- dedupe: one dispatch per assistant message node ------------------
  // WeakSet keyed on the node means a re-render that produces a *new* node
  // is legitimately re-processed, while a re-scan of the same node is not.
  var processed = new WeakSet();
  var iteration = 0;

  function nodeText(node) {
    // innerText when available so markdown renders the same way the user sees
    // it; textContent as fallback for detached or hidden nodes.
    return (node.innerText || node.textContent || "");
  }

  function processAssistantNode(node) {
    if (processed.has(node)) return;
    processed.add(node);

    var text = nodeText(node);
    if (!text) return;

    var blocks = extractFencedJson(text);
    if (blocks.length === 0) {
      log("assistant message has no json block -> prose, loop idle");
      iteration = 0;
      return;
    }

    for (var i = 0; i < blocks.length; i++) {
      var b = blocks[i];
      if (!isRelayCommand(b)) {
        log("fenced json that is not a relay command -> skip");
        continue;
      }
      if (iteration >= CONFIG.maxIterations) {
        log("iteration cap " + CONFIG.maxIterations + " reached -> halted");
        return;
      }
      iteration++;
      log("dispatch #" + iteration + " (" + b.length + " chars)");
      try { window.NexonRelay.run(b); }
      catch (e) { log("bridge missing: " + e.message); }
      return; // exactly one command per assistant message
    }
  }

  function scan(root) {
    var nodes;
    try { nodes = (root || document).querySelectorAll(SITE.assistantSelector); }
    catch (e) { return; }
    for (var i = 0; i < nodes.length; i++) processAssistantNode(nodes[i]);
  }

  // ---- observer -----------------------------------------------------------
  // Any DOM mutation re-arms a quiet timer. When nothing has changed for
  // streamQuietMs, the assistant has stopped streaming and we scan. Premature
  // wakeups are harmless: the brace scanner will refuse an incomplete block.
  var debounceTimer = null;
  function armScan() {
    if (debounceTimer) clearTimeout(debounceTimer);
    debounceTimer = setTimeout(function () {
      debounceTimer = null;
      scan(document);
    }, CONFIG.streamQuietMs);
  }

  var mo = new MutationObserver(function (mutations) {
    for (var i = 0; i < mutations.length; i++) {
      var m = mutations[i];
      if (m.addedNodes && m.addedNodes.length) { armScan(); return; }
      if (m.type === "characterData") { armScan(); return; }
    }
  });
  mo.observe(document.documentElement, {
    childList: true, subtree: true, characterData: true
  });

  // The native side calls this once a command's stdout comes back. relay.js
  // does not type — it fires a DOM event so the InputBackend layer can pick
  // it up without the detector needing to know how input works.
  // Native calls this with the /run envelope. Parse it, extract raw stdout,
  // and ask the native side to type it into the composer and submit.
  window.__nexonRelayResult = function (jsonText) {
    log("result in (" + String(jsonText).length + " chars)");
    var env;
    try { env = JSON.parse(jsonText); } catch (e) {
      log("result not json: " + e.message);
      return;
    }
    if (!env.ok) {
      log("relay error: " + (env.err || "unknown"));
      return;
    }
    var stdout = env.stdout || "";
    if (!stdout.trim()) {
      log("empty stdout -> nothing to type");
      return;
    }
    try { window.NexonRelay.typeAndSend(stdout); }
    catch (e) { log("typeAndSend missing: " + e.message); }
  };

  log("installed on " + location.host);
})();
