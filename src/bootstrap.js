/* WinterCG-ish globals layered on top of the native bindings installed by
   sxn_install_network (the __sxn* functions below). Pure parsing/spec logic
   lives here in JS; anything needing tight C integration (the streaming
   fetch body, random bytes, digests, the monotonic clock) is a thin native
   primitive that this file wraps in a spec-shaped class. */
/* The build compiles this file with qjsc rather than JS_Eval, and qjsc has
   no flag for strict mode, so the directive has to be in the source. */
"use strict";
(function () {
  function domError(name, message) {
    var e = new Error(message);
    e.name = name;
    return e;
  }

  // ---------------- TextEncoder / TextDecoder ----------------
  // The actual byte-at-a-time encode/decode loops are native (__sxnUtf8Encode
  // / __sxnUtf8Decode in src/network.c) -- this class stays JS-visible only
  // for spec surface (construction, .encoding, argument validation).
  function TextEncoder() {}
  Object.defineProperty(TextEncoder.prototype, "encoding", { get: function () { return "utf-8"; } });
  // Bound directly to the native encode: the undefined->empty and coercion
  // handling moved into __sxnUtf8Encode itself (src/network.c), so encode()
  // costs zero interpreted frames per call.
  TextEncoder.prototype.encode = __sxnUtf8Encode;
  // Native: writes straight into `dest` with no intermediate allocation, and
  // reports the code units actually consumed rather than assuming all of them.
  TextEncoder.prototype.encodeInto = __sxnUtf8EncodeInto;
  globalThis.TextEncoder = TextEncoder;

  function TextDecoder(label, options) {
    this._fatal = !!(options && options.fatal);
    this._pending = new Uint8Array(0);
  }
  Object.defineProperty(TextDecoder.prototype, "encoding", { get: function () { return "utf-8"; } });
  TextDecoder.prototype.decode = function (input, options) {
    var stream = !!(options && options.stream);
    var bytes;
    if (input === undefined) bytes = new Uint8Array(0);
    else if (input instanceof Uint8Array) bytes = input;
    else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
    else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    else throw new TypeError("TextDecoder.decode expects a BufferSource");
    if (this._pending.length) {
      var combined = new Uint8Array(this._pending.length + bytes.length);
      combined.set(this._pending);
      combined.set(bytes, this._pending.length);
      bytes = combined;
    }
    var result = __sxnUtf8Decode(bytes, this._fatal, stream);
    this._pending = result.pending;
    return result.text;
  };
  globalThis.TextDecoder = TextDecoder;

  // ---------------- btoa / atob ----------------
  // btoa/atob operate on "binary strings" (one byte per UTF-16 code unit,
  // 0-255) per spec, NOT UTF-8 text -- reuses quickjs-ng's native
  // Uint8Array<->base64 methods as the codec.
  globalThis.btoa = function (data) {
    var str = String(data);
    var bytes = new Uint8Array(str.length);
    for (var i = 0; i < str.length; i++) {
      var code = str.charCodeAt(i);
      if (code > 255) throw domError("InvalidCharacterError", "String contains characters outside of the Latin1 range.");
      bytes[i] = code;
    }
    return bytes.toBase64();
  };
  globalThis.atob = function (data) {
    var bytes;
    try {
      bytes = Uint8Array.fromBase64(String(data));
    } catch (e) {
      throw domError("InvalidCharacterError", "Invalid base64 string.");
    }
    var out = "";
    for (var i = 0; i < bytes.length; i++) out += String.fromCharCode(bytes[i]);
    return out;
  };

  // ---------------- URLSearchParams ----------------
  function decodeComponent(s) {
    return decodeURIComponent(s.replace(/\+/g, " "));
  }
  function encodeComponent(s) {
    return encodeURIComponent(s).replace(/%20/g, "+");
  }
  function URLSearchParams(init) {
    this._pairs = [];
    this._onchange = null;
    if (init === undefined || init === null) {
      // empty
    } else if (typeof init === "string") {
      var s = init[0] === "?" ? init.slice(1) : init;
      if (s.length) {
        var parts = s.split("&");
        for (var i = 0; i < parts.length; i++) {
          if (!parts[i]) continue;
          var eq = parts[i].indexOf("=");
          var k = eq === -1 ? parts[i] : parts[i].slice(0, eq);
          var v = eq === -1 ? "" : parts[i].slice(eq + 1);
          this._pairs.push([decodeComponent(k), decodeComponent(v)]);
        }
      }
    } else if (init instanceof URLSearchParams) {
      for (var j = 0; j < init._pairs.length; j++) this._pairs.push(init._pairs[j].slice());
    } else if (Array.isArray(init)) {
      for (var m = 0; m < init.length; m++) this._pairs.push([String(init[m][0]), String(init[m][1])]);
    } else if (typeof init === "object") {
      var keys = Object.keys(init);
      for (var n = 0; n < keys.length; n++) this._pairs.push([keys[n], String(init[keys[n]])]);
    }
  }
  URLSearchParams.prototype._changed = function () { if (this._onchange) this._onchange(); };
  URLSearchParams.prototype.append = function (name, value) {
    this._pairs.push([String(name), String(value)]);
    this._changed();
  };
  URLSearchParams.prototype.delete = function (name, value) {
    name = String(name);
    if (value === undefined) {
      this._pairs = this._pairs.filter(function (p) { return p[0] !== name; });
    } else {
      value = String(value);
      this._pairs = this._pairs.filter(function (p) { return !(p[0] === name && p[1] === value); });
    }
    this._changed();
  };
  URLSearchParams.prototype.get = function (name) {
    name = String(name);
    for (var i = 0; i < this._pairs.length; i++) if (this._pairs[i][0] === name) return this._pairs[i][1];
    return null;
  };
  URLSearchParams.prototype.getAll = function (name) {
    name = String(name);
    return this._pairs.filter(function (p) { return p[0] === name; }).map(function (p) { return p[1]; });
  };
  URLSearchParams.prototype.has = function (name, value) {
    name = String(name);
    if (value === undefined) return this._pairs.some(function (p) { return p[0] === name; });
    value = String(value);
    return this._pairs.some(function (p) { return p[0] === name && p[1] === value; });
  };
  URLSearchParams.prototype.set = function (name, value) {
    name = String(name); value = String(value);
    var found = false;
    this._pairs = this._pairs.filter(function (p) {
      if (p[0] === name) {
        if (found) return false;
        p[1] = value; found = true; return true;
      }
      return true;
    });
    if (!found) this._pairs.push([name, value]);
    this._changed();
  };
  URLSearchParams.prototype.sort = function () {
    this._pairs.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
    this._changed();
  };
  URLSearchParams.prototype.forEach = function (cb, thisArg) {
    for (var i = 0; i < this._pairs.length; i++) cb.call(thisArg, this._pairs[i][1], this._pairs[i][0], this);
  };
  URLSearchParams.prototype.keys = function () { return this._pairs.map(function (p) { return p[0]; })[Symbol.iterator](); };
  URLSearchParams.prototype.values = function () { return this._pairs.map(function (p) { return p[1]; })[Symbol.iterator](); };
  URLSearchParams.prototype.entries = function () { return this._pairs.map(function (p) { return p.slice(); })[Symbol.iterator](); };
  URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;
  URLSearchParams.prototype.toString = function () {
    return this._pairs.map(function (p) { return encodeComponent(p[0]) + "=" + encodeComponent(p[1]); }).join("&");
  };
  globalThis.URLSearchParams = URLSearchParams;

  // ---------------- URL ----------------
  function resolveRelative(base, input) {
    if (input.indexOf("//") === 0) return base._protocol + input;
    if (input[0] === "/") return base._protocol + "//" + base.host + input;
    if (input[0] === "?") return base._protocol + "//" + base.host + base._pathname + input;
    if (input[0] === "#") return base.toString().split("#")[0] + input;
    var dir = base._pathname.slice(0, base._pathname.lastIndexOf("/") + 1) || "/";
    return base._protocol + "//" + base.host + dir + input;
  }
  var URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*):(\/\/)?([^\/?#]*)?([^?#]*)?(\?[^#]*)?(#.*)?$/;
  function URL(input, base) {
    input = String(input);
    if (base !== undefined && base !== null && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(input)) {
      var baseUrl = base instanceof URL ? base : new URL(String(base));
      input = resolveRelative(baseUrl, input);
    }
    var m = URL_RE.exec(input);
    if (!m) throw new TypeError("Invalid URL: " + input);
    this._protocol = m[1].toLowerCase() + ":";
    var authority = m[3] || "";
    this._pathname = m[4] || (m[2] ? "/" : "");
    this._search = m[5] || "";
    this._hash = m[6] || "";
    this._username = ""; this._password = ""; this._host = ""; this._port = "";
    if (authority) {
      var hostpart = authority;
      var at = authority.lastIndexOf("@");
      if (at !== -1) {
        var userinfo = authority.slice(0, at);
        hostpart = authority.slice(at + 1);
        var uc = userinfo.indexOf(":");
        if (uc === -1) this._username = userinfo;
        else { this._username = userinfo.slice(0, uc); this._password = userinfo.slice(uc + 1); }
      }
      if (hostpart[0] === "[") {
        var end = hostpart.indexOf("]");
        this._host = hostpart.slice(0, end + 1);
        var rest = hostpart.slice(end + 1);
        if (rest[0] === ":") this._port = rest.slice(1);
      } else {
        var hc = hostpart.indexOf(":");
        if (hc === -1) this._host = hostpart;
        else { this._host = hostpart.slice(0, hc); this._port = hostpart.slice(hc + 1); }
      }
      if (!this._pathname) this._pathname = "/";
    }
    this._searchParamsObj = null;
  }
  Object.defineProperty(URL.prototype, "protocol", {
    get: function () { return this._protocol; },
    set: function (v) { this._protocol = String(v).replace(/:$/, "") + ":"; }
  });
  Object.defineProperty(URL.prototype, "username", {
    get: function () { return this._username; },
    set: function (v) { this._username = String(v); }
  });
  Object.defineProperty(URL.prototype, "password", {
    get: function () { return this._password; },
    set: function (v) { this._password = String(v); }
  });
  Object.defineProperty(URL.prototype, "hostname", {
    get: function () { return this._host; },
    set: function (v) { this._host = String(v); }
  });
  Object.defineProperty(URL.prototype, "port", {
    get: function () { return this._port; },
    set: function (v) { this._port = String(v); }
  });
  Object.defineProperty(URL.prototype, "host", {
    get: function () { return this._port ? this._host + ":" + this._port : this._host; },
    set: function (v) {
      v = String(v);
      var c = v.lastIndexOf(":");
      if (c === -1) { this._host = v; this._port = ""; }
      else { this._host = v.slice(0, c); this._port = v.slice(c + 1); }
    }
  });
  Object.defineProperty(URL.prototype, "pathname", {
    get: function () { return this._pathname; },
    set: function (v) { this._pathname = String(v); }
  });
  Object.defineProperty(URL.prototype, "search", {
    get: function () { return this._search; },
    set: function (v) {
      v = String(v);
      this._search = v && v[0] !== "?" ? "?" + v : v;
      if (this._searchParamsObj) {
        var fresh = new URLSearchParams(this._search);
        this._searchParamsObj._pairs = fresh._pairs;
      }
    }
  });
  Object.defineProperty(URL.prototype, "hash", {
    get: function () { return this._hash; },
    set: function (v) { v = String(v); this._hash = v && v[0] !== "#" ? "#" + v : v; }
  });
  Object.defineProperty(URL.prototype, "origin", {
    get: function () { return this._protocol + "//" + this.host; }
  });
  Object.defineProperty(URL.prototype, "searchParams", {
    get: function () {
      if (!this._searchParamsObj) {
        var self = this;
        this._searchParamsObj = new URLSearchParams(this._search);
        this._searchParamsObj._onchange = function () {
          var s = self._searchParamsObj.toString();
          self._search = s ? "?" + s : "";
        };
      }
      return this._searchParamsObj;
    }
  });
  Object.defineProperty(URL.prototype, "href", {
    get: function () { return this.toString(); },
    set: function (v) {
      var u = new URL(String(v));
      this._protocol = u._protocol; this._username = u._username; this._password = u._password;
      this._host = u._host; this._port = u._port; this._pathname = u._pathname;
      this._search = u._search; this._hash = u._hash; this._searchParamsObj = null;
    }
  });
  URL.prototype.toString = function () {
    var authority = "";
    if (this._username || this._password) {
      authority += this._username;
      if (this._password) authority += ":" + this._password;
      authority += "@";
    }
    authority += this._host;
    if (this._port) authority += ":" + this._port;
    var slashes = authority || this._pathname.indexOf("/") === 0 ? "//" : "";
    return this._protocol + slashes + authority + this._pathname + this._search + this._hash;
  };
  URL.prototype.toJSON = function () { return this.toString(); };
  // Parses-or-not without the cost and noise of a thrown exception.
  URL.canParse = function (url, base) {
    try { new URL(url, base); return true; } catch { return false; }
  };
  URL.parse = function (url, base) {
    try { return new URL(url, base); } catch { return null; }
  };
  globalThis.URL = URL;

  // ---------------- Headers ----------------
  var HEADER_NAME_RE = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
  function Headers(init) {
    this._list = [];
    if (init === undefined || init === null) {
      // empty
    } else if (init instanceof Headers) {
      for (var i = 0; i < init._list.length; i++) this._list.push(init._list[i].slice());
    } else if (Array.isArray(init)) {
      for (var j = 0; j < init.length; j++) this.append(init[j][0], init[j][1]);
    } else if (typeof init === "object") {
      var keys = Object.keys(init);
      for (var k = 0; k < keys.length; k++) this.append(keys[k], init[keys[k]]);
    }
  }
  Headers.prototype._norm = function (name) {
    name = String(name);
    if (!HEADER_NAME_RE.test(name)) throw new TypeError("Invalid header name: " + name);
    return name.toLowerCase();
  };
  Headers.prototype.append = function (name, value) {
    name = this._norm(name); value = String(value);
    // Set-Cookie is the one header that never combines: two cookies joined by
    // a comma are a single malformed cookie, not two. Every other repeated
    // name folds into one comma-separated value, per the fetch standard.
    if (name !== "set-cookie") {
      for (var i = 0; i < this._list.length; i++) {
        if (this._list[i][0] === name) { this._list[i][1] += ", " + value; return; }
      }
    }
    this._list.push([name, value]);
  };
  Headers.prototype.set = function (name, value) {
    name = this._norm(name); value = String(value);
    var found = false;
    this._list = this._list.filter(function (p) {
      if (p[0] === name) { if (found) return false; p[1] = value; found = true; return true; }
      return true;
    });
    if (!found) this._list.push([name, value]);
  };
  Headers.prototype.get = function (name) {
    name = this._norm(name);
    for (var i = 0; i < this._list.length; i++) if (this._list[i][0] === name) return this._list[i][1];
    return null;
  };
  Headers.prototype.has = function (name) {
    name = this._norm(name);
    return this._list.some(function (p) { return p[0] === name; });
  };
  Headers.prototype.delete = function (name) {
    name = this._norm(name);
    this._list = this._list.filter(function (p) { return p[0] !== name; });
  };
  function sortedHeaderList(self) {
    return self._list.slice().sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
  }
  Headers.prototype.forEach = function (cb, thisArg) {
    var sorted = sortedHeaderList(this);
    for (var i = 0; i < sorted.length; i++) cb.call(thisArg, sorted[i][1], sorted[i][0], this);
  };
  Headers.prototype.keys = function () { return sortedHeaderList(this).map(function (p) { return p[0]; })[Symbol.iterator](); };
  Headers.prototype.values = function () { return sortedHeaderList(this).map(function (p) { return p[1]; })[Symbol.iterator](); };
  Headers.prototype.entries = function () { return sortedHeaderList(this).map(function (p) { return p.slice(); })[Symbol.iterator](); };
  Headers.prototype[Symbol.iterator] = Headers.prototype.entries;
  // Set-Cookie is the one header that must not be joined into a single
  // comma-separated value, so the standard exposes it separately.
  Headers.prototype.getSetCookie = function () {
    const out = [];
    for (const pair of this._list) if (pair[0].toLowerCase() === "set-cookie") out.push(pair[1]);
    return out;
  };
  globalThis.Headers = Headers;

  // ---------------- Error.prepareStackTrace ----------------
  // V8's structured stack API. Packages that inspect call sites -- depd, and
  // the deprecation paths inside Express -- set Error.prepareStackTrace and
  // expect CallSite objects rather than a string. The frames are parsed back
  // out of the engine's own stack text, so the information is real, just
  // recovered rather than captured natively.
  const FRAME_RE = /^\s*at\s+(?:(.*?)\s+\()?([^()]*?):(\d+):(\d+)\)?$/;
  function parseCallSites(stackText) {
    const sites = [];
    for (const line of String(stackText || "").split("\n")) {
      const m = FRAME_RE.exec(line);
      if (!m) continue;
      const fn = m[1] || null, file = m[2], ln = Number(m[3]), col = Number(m[4]);
      sites.push({
        getFileName: () => (file === "native" ? null : file),
        getLineNumber: () => ln,
        getColumnNumber: () => col,
        getFunctionName: () => fn,
        getMethodName: () => fn,
        getTypeName: () => null,
        getThis: () => undefined,
        getFunction: () => undefined,
        getEvalOrigin: () => undefined,
        isEval: () => false,
        isNative: () => file === "native",
        isConstructor: () => false,
        isToplevel: () => !fn,
        isAsync: () => false,
        toString: () => (fn ? fn + " (" + file + ":" + ln + ":" + col + ")"
                            : file + ":" + ln + ":" + col),
      });
    }
    return sites;
  }

  const nativeCapture = Error.captureStackTrace;
  Error.captureStackTrace = function (target, constructorOpt) {
    if (typeof nativeCapture === "function") {
      // Pass the second argument only when there is one: the native form
      // treats an explicit undefined differently from an absent argument.
      try {
        if (constructorOpt === undefined) nativeCapture(target);
        else nativeCapture(target, constructorOpt);
      } catch { /* keep going */ }
    }
    // Only take over when the program has asked for structured frames.
    if (typeof Error.prepareStackTrace === "function") {
      const raw = target.stack;
      let sites = parseCallSites(raw);
      if (constructorOpt && constructorOpt.name) {
        const cut = sites.findIndex((s) => s.getFunctionName() === constructorOpt.name);
        if (cut >= 0) sites = sites.slice(cut + 1);
      }
      // If nothing parsed, leave the string alone rather than handing the
      // program an empty array it will index into.
      if (sites.length > 0) {
        try { target.stack = Error.prepareStackTrace(target, sites); }
        catch { target.stack = raw; }
      }
    }
    return target;
  };

  // ---------------- Event / EventTarget / CustomEvent ----------------
  // The DOM event pattern, which plenty of runtime-agnostic packages expect
  // even outside a browser: tinybench extends EventTarget to report progress.
  // There is no node to bubble through here, so capture and bubbling are
  // accepted and ignored; everything observable on a single target behaves.
  function Event(type, init) {
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.defaultPrevented = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.isTrusted = false;
    this.timeStamp = performance.now();
    this._stop = false;
    this._stopNow = false;
  }
  Event.prototype.preventDefault = function () { if (this.cancelable) this.defaultPrevented = true; };
  Event.prototype.stopPropagation = function () { this._stop = true; };
  Event.prototype.stopImmediatePropagation = function () { this._stop = true; this._stopNow = true; };
  Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
  globalThis.Event = Event;

  function CustomEvent(type, init) {
    Event.call(this, type, init);
    this.detail = init && "detail" in init ? init.detail : null;
  }
  CustomEvent.prototype = Object.create(Event.prototype);
  CustomEvent.prototype.constructor = CustomEvent;
  globalThis.CustomEvent = CustomEvent;

  function EventTarget() { Object.defineProperty(this, "_listeners", { value: new Map(), writable: true, configurable: true }); }
  function listenersOf(self, type) {
    if (!self._listeners) Object.defineProperty(self, "_listeners", { value: new Map(), writable: true, configurable: true });
    let l = self._listeners.get(type);
    if (!l) { l = []; self._listeners.set(type, l); }
    return l;
  }
  EventTarget.prototype.addEventListener = function (type, callback, options) {
    if (callback === null || callback === undefined) return;
    type = String(type);
    const opts = typeof options === "boolean" ? { capture: options } : (options || {});
    const list = listenersOf(this, type);
    // A duplicate (callback, capture) pair is ignored, per the spec.
    for (const e of list) if (e.callback === callback && e.capture === !!opts.capture) return;
    const entry = { callback: callback, once: !!opts.once, capture: !!opts.capture, signal: opts.signal };
    list.push(entry);
    if (opts.signal) {
      if (opts.signal.aborted) { const i = list.indexOf(entry); if (i >= 0) list.splice(i, 1); return; }
      if (typeof opts.signal.addEventListener === "function")
        opts.signal.addEventListener("abort", () => { const i = list.indexOf(entry); if (i >= 0) list.splice(i, 1); });
    }
  };
  EventTarget.prototype.removeEventListener = function (type, callback, options) {
    type = String(type);
    const capture = typeof options === "boolean" ? options : !!(options && options.capture);
    const list = this._listeners && this._listeners.get(type);
    if (!list) return;
    for (let i = 0; i < list.length; i++)
      if (list[i].callback === callback && list[i].capture === capture) { list.splice(i, 1); return; }
  };
  EventTarget.prototype.dispatchEvent = function (event) {
    if (!event || typeof event.type !== "string")
      throw new TypeError("dispatchEvent requires an Event");
    event.target = this;
    event.currentTarget = this;
    event.eventPhase = 2;
    const list = this._listeners && this._listeners.get(event.type);
    // Walked live rather than over a copy: a listener registered during
    // dispatch runs in that same dispatch, and one removed during it does
    // not. That is what Node does, and what packages are written against.
    if (list) {
      for (let i = 0; i < list.length; ) {
        if (event._stopNow) break;
        const entry = list[i];
        if (entry.once) list.splice(i, 1); else i++;
        const fn = typeof entry.callback === "function" ? entry.callback
                 : (entry.callback && typeof entry.callback.handleEvent === "function"
                    ? entry.callback.handleEvent.bind(entry.callback) : null);
        if (fn) fn.call(this, event);
      }
    }
    event.currentTarget = null;
    event.eventPhase = 0;
    return !event.defaultPrevented;
  };
  globalThis.EventTarget = EventTarget;

  // ---------------- AbortController / AbortSignal ----------------
  function AbortSignal() {
    this._aborted = false;
    this._reason = undefined;
    this._listeners = [];
  }
  Object.defineProperty(AbortSignal.prototype, "aborted", { get: function () { return this._aborted; } });
  Object.defineProperty(AbortSignal.prototype, "reason", { get: function () { return this._reason; } });
  AbortSignal.prototype.addEventListener = function (type, cb) {
    if (type === "abort" && this._listeners.indexOf(cb) === -1) this._listeners.push(cb);
  };
  AbortSignal.prototype.removeEventListener = function (type, cb) {
    if (type === "abort") this._listeners = this._listeners.filter(function (l) { return l !== cb; });
  };
  AbortSignal.prototype.throwIfAborted = function () { if (this._aborted) throw this._reason; };
  AbortSignal.prototype._doAbort = function (reason) {
    if (this._aborted) return;
    this._aborted = true;
    this._reason = reason !== undefined ? reason : domError("AbortError", "The operation was aborted.");
    var listeners = this._listeners.slice();
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i](); } catch (e) { /* listener errors don't propagate */ }
    }
  };
  globalThis.AbortSignal = AbortSignal;

  function AbortController() { this.signal = new AbortSignal(); }
  AbortController.prototype.abort = function (reason) { this.signal._doAbort(reason); };
  globalThis.AbortController = AbortController;

  // ---------------- Blob (WinterCG) ----------------
  // Bytes are concatenated eagerly at construction time into a single
  // Uint8Array (this._bytes) -- a deliberately minimal, honestly-scoped
  // in-memory backing store rather than a lazy/streamed one, matching the
  // scope of .stream() below. String parts are UTF-8-encoded via the native
  // __sxnUtf8Encode primitive (TextEncoder's own primitive), not reimplemented.
  // Both of these hand back a real ReadableStream, so a response body or a
  // Blob supports tee, pipeThrough and `for await` like any other stream --
  // the duck-typed object they used to return supported only getReader.
  function singleChunkStream(bytes) {
    return new ReadableStream({
      start(c) { c.enqueue(bytes); c.close(); },
    });
  }

  /* Wraps the native fetch stream (which exposes only getReader) so a
     response body is an actual ReadableStream. */
  function wrapNativeStream(raw) {
    if (raw instanceof ReadableStream) return raw;
    let reader = null;
    return new ReadableStream({
      start() { reader = raw.getReader(); },
      async pull(c) {
        const { value, done } = await reader.read();
        if (done) c.close(); else c.enqueue(value);
      },
      cancel(reason) { return reader && reader.cancel ? reader.cancel(reason) : undefined; },
    });
  }
  function blobPartBytes(part) {
    if (part instanceof Blob) return part._bytes;
    if (part instanceof ArrayBuffer) return new Uint8Array(part);
    if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
    return __sxnUtf8Encode(String(part));
  }
  // Matches real Node/browser behavior (checked via node -e): type is
  // lowercased, and reset to "" if it contains anything outside printable
  // ASCII (0x20-0x7E) -- but an otherwise-odd-looking ASCII type like
  // "bad type" is kept as-is, only lowercased, not otherwise validated.
  function normalizeBlobType(type) {
    if (typeof type !== "string") return "";
    for (var i = 0; i < type.length; i++) {
      var c = type.charCodeAt(i);
      if (c < 0x20 || c > 0x7e) return "";
    }
    return type.toLowerCase();
  }
  function Blob(parts, options) {
    var pieces = [];
    var total = 0;
    if (parts !== undefined && parts !== null) {
      for (var i = 0; i < parts.length; i++) {
        var bytes = blobPartBytes(parts[i]);
        pieces.push(bytes);
        total += bytes.length;
      }
    }
    var out = new Uint8Array(total);
    var offset = 0;
    for (var j = 0; j < pieces.length; j++) { out.set(pieces[j], offset); offset += pieces[j].length; }
    this._bytes = out;
    this.type = normalizeBlobType(options && options.type);
  }
  Blob.prototype[Symbol.toStringTag] = "Blob";
  Object.defineProperty(Blob.prototype, "size", { get: function () { return this._bytes.length; } });
  // Real Blob.slice(start, end, contentType) semantics (checked via node -e):
  // omitting contentType resets the slice's type to "" -- it is NOT
  // inherited from the parent Blob. subarray() keeps this zero-copy: the
  // slice shares its parent's backing ArrayBuffer.
  Blob.prototype.slice = function (start, end, contentType) {
    var len = this._bytes.length;
    var s = start === undefined ? 0 : (start < 0 ? Math.max(len + start, 0) : Math.min(start, len));
    var e = end === undefined ? len : (end < 0 ? Math.max(len + end, 0) : Math.min(end, len));
    var out = new Blob([]);
    out._bytes = this._bytes.subarray(s, Math.max(s, e));
    out.type = normalizeBlobType(contentType);
    return out;
  };
  Blob.prototype.text = function () { return Promise.resolve(new TextDecoder().decode(this._bytes)); };
  Blob.prototype.arrayBuffer = function () { return Promise.resolve(this._bytes.slice().buffer); };
  // A minimal, honestly-scoped single-chunk ReadableStream-shaped object
  // (getReader/read/cancel, matching SxnBodyStream's reader shape) rather
  // than a full ReadableStream implementation -- this codebase has no
  // ReadableStream class to build one atop of, and a Blob's bytes are
  // already fully in memory, so a real multi-chunk stream would add
  // complexity with no behavioral benefit here.
  Blob.prototype.stream = function () { return singleChunkStream(this._bytes); };
  globalThis.Blob = Blob;

  // ---------------- Request / Response ----------------
  function Request(input, init) {
    init = init || {};
    if (input instanceof Request) {
      this.url = input.url;
      this.method = input.method;
      this.headers = new Headers(input.headers);
      this._body = input._body;
      this.signal = input.signal || null;
    } else {
      this.url = String(input);
      this.method = "GET";
      this.headers = new Headers();
      this._body = null;
      this.signal = null;
    }
    if (init.method !== undefined) this.method = String(init.method).toUpperCase();
    if (init.headers !== undefined) this.headers = new Headers(init.headers);
    if (init.body !== undefined) this._body = init.body;
    if (init.signal !== undefined) this.signal = init.signal;
  }
  Object.defineProperty(Request.prototype, "body", { get: function () { return null; } });
  Request.prototype.clone = function () { return new Request(this); };
  globalThis.Request = Request;

  function Response(body, init) {
    init = init || {};
    this._rawStream = null;
    this._staticBody = undefined;
    this._staticBytes = undefined;
    this._bodyUsed = false;
    this.status = init.status !== undefined ? init.status : 200;
    this.statusText = init.statusText || "";
    this.headers = new Headers(init.headers);
    this.ok = this.status >= 200 && this.status < 300;
    this.url = init.url || "";
    this.type = "basic";
    this.redirected = false;
    if (body !== null && body !== undefined && typeof body === "object" && typeof body.getReader === "function") {
      this._rawStream = body;
    } else if (body instanceof Blob) {
      // Real fetch/undici behavior (checked via node -e): a Blob body with a
      // non-empty .type sets Content-Type automatically, unless the caller
      // already set one; a Uint8Array/ArrayBuffer body does NOT do this.
      this._staticBytes = body._bytes;
      if (body.type && !this.headers.has("content-type")) this.headers.set("content-type", body.type);
    } else if (body instanceof Uint8Array) {
      this._staticBytes = body;
    } else if (body instanceof ArrayBuffer) {
      this._staticBytes = new Uint8Array(body);
    } else if (body !== null && body !== undefined) {
      this._staticBody = String(body);
    } else {
      this._staticBody = "";
    }
  }
  Object.defineProperty(Response.prototype, "body", {
    get: function () {
      if (this._rawStream) {
        if (!this._wrappedStream) this._wrappedStream = wrapNativeStream(this._rawStream);
        return this._wrappedStream;
      }
      if (this._staticBytes !== undefined) return singleChunkStream(this._staticBytes);
      // A string body is still a body: Node exposes it as a stream of its
      // utf-8 bytes rather than null.
      if (this._staticBody !== undefined && this._staticBody !== null)
        return singleChunkStream(new TextEncoder().encode(this._staticBody));
      return null;
    }
  });
  Object.defineProperty(Response.prototype, "bodyUsed", { get: function () { return this._bodyUsed; } });
  Response.prototype._readAll = async function () {
    this._bodyUsed = true;
    if (this._staticBytes !== undefined) return new TextDecoder().decode(this._staticBytes);
    if (this._staticBody !== undefined) return this._staticBody;
    if (!this._rawStream) return "";
    var decoder = new TextDecoder();
    var out = "";
    var reader = this._rawStream.getReader();
    for (;;) {
      var r = await reader.read();
      if (r.done) break;
      out += decoder.decode(r.value, { stream: true });
    }
    out += decoder.decode();
    return out;
  };
  Response.prototype.text = function () { return this._readAll(); };
  Response.prototype.json = async function () { return JSON.parse(await this._readAll()); };
  Response.prototype.arrayBuffer = async function () {
    this._bodyUsed = true;
    if (this._staticBytes !== undefined) return this._staticBytes.slice().buffer;
    if (this._staticBody !== undefined) return new TextEncoder().encode(this._staticBody).buffer;
    if (!this._rawStream) return new ArrayBuffer(0);
    var chunks = []; var total = 0;
    var reader = this._rawStream.getReader();
    for (;;) {
      var r = await reader.read();
      if (r.done) break;
      chunks.push(r.value); total += r.value.length;
    }
    var out = new Uint8Array(total); var off = 0;
    for (var i = 0; i < chunks.length; i++) { out.set(chunks[i], off); off += chunks[i].length; }
    return out.buffer;
  };
  Response.prototype.blob = async function () {
    var buf = await this.arrayBuffer();
    return new Blob([buf], { type: this.headers.get("content-type") || "" });
  };
  Response.prototype.clone = function () { throw new Error("Response.clone() is not supported"); };
  // Static constructors and clone, which the fetch standard requires and
  // handlers reach for constantly.
  Response.json = function (data, init) {
    init = init || {};
    const h = new Headers(init.headers);
    if (!h.has("content-type")) h.set("content-type", "application/json");
    return new Response(JSON.stringify(data), { status: init.status, statusText: init.statusText, headers: h });
  };
  Response.error = function () {
    const r = new Response(null, { status: 0 });
    r.type = "error";
    return r;
  };
  Response.redirect = function (url, status) {
    status = status === undefined ? 302 : status;
    if ([301, 302, 303, 307, 308].indexOf(status) < 0)
      throw new RangeError("invalid redirect status " + status);
    return new Response(null, { status: status, headers: { location: String(new URL(url, "http://localhost/")) } });
  };
  Response.prototype.clone = function () {
    if (this._bodyUsed) throw new TypeError("Response body is already used");
    const r = new Response(this._staticBytes !== undefined ? this._staticBytes : this._staticBody,
                           { status: this.status, statusText: this.statusText, headers: this.headers });
    r.url = this.url; r.type = this.type; r.redirected = this.redirected;
    return r;
  };
  globalThis.Response = Response;

  // ---------------- fetch() ----------------
  function headerPairsFromHeaders(headers) {
    var flat = [];
    var h = headers instanceof Headers ? headers : new Headers(headers);
    for (var i = 0; i < h._list.length; i++) { flat.push(h._list[i][0]); flat.push(h._list[i][1]); }
    return flat;
  }
  globalThis.fetch = function (input, init) {
    init = init || {};
    var request = input instanceof Request ? input : new Request(input, init);
    var signal = init.signal !== undefined ? init.signal : request.signal;
    if (signal && signal.aborted) {
      return Promise.reject(signal.reason !== undefined ? signal.reason : domError("AbortError", "The operation was aborted."));
    }
    var body = request._body !== undefined && request._body !== null ? String(request._body) : undefined;
    var raw = __sxnFetchRaw(request.url, request.method, headerPairsFromHeaders(request.headers), body);
    if (signal) signal.addEventListener("abort", raw.stream.__abort.bind(raw.stream));
    return raw.promise.then(function (head) {
      var headers = new Headers();
      for (var i = 0; i < head.headers.length; i += 2) headers.append(head.headers[i], head.headers[i + 1]);
      return new Response(raw.stream, { status: head.status, statusText: head.statusText, headers: headers, url: head.url });
    });
  };

  // ---------------- setTimeout / setInterval ----------------
  // Node coerces a missing/non-positive/NaN delay to 1ms rather than 0 or an
  // error (confirmed against real Node: setTimeout(fn), setTimeout(fn, 0),
  // setTimeout(fn, undefined), setTimeout(fn, -5) and setTimeout(fn, NaN)
  // all schedule at delay=1).
  function normalizeDelay(delay) {
    var n = Number(delay);
    return n >= 1 ? n : 1;
  }
  function scheduleTimer(repeat, fn, delay, extraArgs) {
    if (typeof fn !== "function") throw new TypeError("callback is not a function");
    return __sxnSetTimer(function () { fn.apply(undefined, extraArgs); }, normalizeDelay(delay), repeat);
  }
  globalThis.setTimeout = function (fn, delay) {
    return scheduleTimer(false, fn, delay, Array.prototype.slice.call(arguments, 2));
  };
  globalThis.setInterval = function (fn, delay) {
    return scheduleTimer(true, fn, delay, Array.prototype.slice.call(arguments, 2));
  };
  globalThis.clearTimeout = function (id) {
    if (id !== undefined && id !== null) __sxnClearTimer(id);
  };
  globalThis.clearInterval = globalThis.clearTimeout;

  // ---------------- performance ----------------
  // Bind the C primitive directly: a JS wrapper here cost an interpreted
  // frame on every call, ~9.8ns of a ~35ns performance.now().
  globalThis.performance = { now: __sxnNow };

  // ---------------- Web Streams ----------------
  // ReadableStream/WritableStream/TransformStream and the pieces around them.
  // Faithful on the paths programs actually take -- backpressure through
  // desiredSize and pull, locking, cancellation, tee, pipeTo/pipeThrough and
  // async iteration. Byte streams are supported as ordinary chunk streams:
  // `type: "bytes"` is accepted, BYOB readers are not.
  const S_READABLE = 0, S_CLOSED = 1, S_ERRORED = 2;

  function streamAssert(cond, msg) { if (!cond) throw new TypeError(msg); }
  function deferred() {
    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    // Nothing here always attaches a handler, and an unobserved rejection is
    // noise rather than a fault, so keep one attached from the start.
    promise.catch(() => {});
    return { promise, resolve, reject };
  }

  function ReadableStreamDefaultController(stream, source, strategy) {
    this._stream = stream;
    this._source = source;
    this._queue = [];
    this._queueSize = 0;
    this._highWaterMark = strategy.highWaterMark === undefined ? 1 : Number(strategy.highWaterMark);
    this._sizeOf = typeof strategy.size === "function" ? strategy.size : () => 1;
    this._closeRequested = false;
    this._pulling = false;
    this._pullAgain = false;
    this._started = false;
  }
  Object.defineProperty(ReadableStreamDefaultController.prototype, "desiredSize", {
    get() {
      const s = this._stream;
      if (s._state === S_ERRORED) return null;
      if (s._state === S_CLOSED) return 0;
      return this._highWaterMark - this._queueSize;
    },
  });
  ReadableStreamDefaultController.prototype.enqueue = function (chunk) {
    const s = this._stream;
    if (this._closeRequested || s._state !== S_READABLE)
      throw new TypeError("stream is not readable");
    // A waiting reader takes the chunk directly; nothing queues behind it.
    if (s._readRequests.length > 0) {
      s._readRequests.shift().resolve({ value: chunk, done: false });
    } else {
      let size = 1;
      try { size = this._sizeOf(chunk); } catch (e) { this.error(e); throw e; }
      this._queue.push(chunk);
      this._queueSize += size;
    }
    controllerPull(this);
  };
  ReadableStreamDefaultController.prototype.close = function () {
    const s = this._stream;
    if (this._closeRequested || s._state !== S_READABLE)
      throw new TypeError("stream is not readable");
    this._closeRequested = true;
    if (this._queue.length === 0) readableClose(s);
  };
  ReadableStreamDefaultController.prototype.error = function (e) {
    readableError(this._stream, e);
  };

  function controllerPull(c) {
    const s = c._stream;
    if (!c._started || s._state !== S_READABLE || c._closeRequested) return;
    const wantsMore = c._queue.length === 0 || c.desiredSize > 0;
    if (!wantsMore) return;
    if (c._pulling) { c._pullAgain = true; return; }
    if (typeof c._source.pull !== "function") return;
    c._pulling = true;
    Promise.resolve().then(() => c._source.pull(c)).then(
      () => {
        c._pulling = false;
        if (c._pullAgain) { c._pullAgain = false; controllerPull(c); }
      },
      (e) => { c._pulling = false; readableError(s, e); }
    );
  }

  function readableClose(s) {
    if (s._state !== S_READABLE) return;
    s._state = S_CLOSED;
    for (const r of s._readRequests.splice(0)) r.resolve({ value: undefined, done: true });
    s._closedDeferred.resolve(undefined);
  }
  function readableError(s, e) {
    if (s._state !== S_READABLE) return;
    s._state = S_ERRORED;
    s._storedError = e;
    s._controller._queue.length = 0;
    s._controller._queueSize = 0;
    for (const r of s._readRequests.splice(0)) r.reject(e);
    s._closedDeferred.reject(e);
  }

  function ReadableStream(source, strategy) {
    source = source || {};
    strategy = strategy || {};
    this._state = S_READABLE;
    this._storedError = undefined;
    this._reader = null;
    this._readRequests = [];
    this._closedDeferred = deferred();
    this._disturbed = false;
    const c = new ReadableStreamDefaultController(this, source, strategy);
    this._controller = c;
    const started = typeof source.start === "function"
      ? Promise.resolve().then(() => source.start(c)) : Promise.resolve();
    started.then(() => { c._started = true; controllerPull(c); },
                 (e) => readableError(this, e));
  }
  Object.defineProperty(ReadableStream.prototype, "locked", {
    get() { return this._reader !== null; },
  });
  ReadableStream.prototype.getReader = function (options) {
    if (options && options.mode === "byob")
      throw new TypeError("byob readers are not supported");
    streamAssert(!this.locked, "stream is already locked");
    return new ReadableStreamDefaultReader(this);
  };
  ReadableStream.prototype.cancel = function (reason) {
    if (this.locked) return Promise.reject(new TypeError("stream is locked"));
    return readableCancel(this, reason);
  };
  function readableCancel(s, reason) {
    s._disturbed = true;
    if (s._state === S_CLOSED) return Promise.resolve(undefined);
    if (s._state === S_ERRORED) return Promise.reject(s._storedError);
    s._controller._queue.length = 0;
    s._controller._queueSize = 0;
    readableClose(s);
    const c = s._controller._source.cancel;
    return Promise.resolve(typeof c === "function" ? c.call(s._controller._source, reason) : undefined)
      .then(() => undefined);
  }

  ReadableStream.prototype.tee = function () {
    streamAssert(!this.locked, "stream is already locked");
    const reader = this.getReader();
    let reading = false, canceled1 = false, canceled2 = false;
    let c1, c2;
    const reason1 = {}, reason2 = {};
    const pump = () => {
      if (reading) return Promise.resolve();
      reading = true;
      return reader.read().then(({ value, done }) => {
        reading = false;
        if (done) {
          if (!canceled1) { try { c1.close(); } catch {} }
          if (!canceled2) { try { c2.close(); } catch {} }
          return;
        }
        if (!canceled1) c1.enqueue(value);
        if (!canceled2) c2.enqueue(value);
      }, (e) => { reading = false; c1.error(e); c2.error(e); });
    };
    const maybeCancel = () => {
      if (canceled1 && canceled2) reader.cancel([reason1.v, reason2.v]);
    };
    const b1 = new ReadableStream({
      start(c) { c1 = c; }, pull: pump,
      cancel(r) { canceled1 = true; reason1.v = r; maybeCancel(); },
    });
    const b2 = new ReadableStream({
      start(c) { c2 = c; }, pull: pump,
      cancel(r) { canceled2 = true; reason2.v = r; maybeCancel(); },
    });
    return [b1, b2];
  };

  ReadableStream.prototype.pipeTo = function (dest, options) {
    options = options || {};
    if (this.locked) return Promise.reject(new TypeError("stream is locked"));
    if (dest.locked) return Promise.reject(new TypeError("destination is locked"));
    const reader = this.getReader();
    const writer = dest.getWriter();
    return new Promise((resolve, reject) => {
      const fail = (e) => {
        reader.releaseLock();
        if (!options.preventAbort) { writer.abort(e).catch(() => {}); }
        writer.releaseLock();
        reject(e);
      };
      const step = () => {
        writer.ready.then(() => reader.read()).then(({ value, done }) => {
          if (done) {
            const finish = options.preventClose ? Promise.resolve() : writer.close();
            return finish.then(() => { reader.releaseLock(); writer.releaseLock(); resolve(undefined); }, fail);
          }
          return writer.write(value).then(step, fail);
        }, fail);
      };
      step();
    });
  };
  ReadableStream.prototype.pipeThrough = function (pair, options) {
    streamAssert(pair && pair.writable && pair.readable, "pipeThrough needs { writable, readable }");
    this.pipeTo(pair.writable, options).catch(() => {});
    return pair.readable;
  };
  ReadableStream.prototype.values = function (options) {
    const reader = this.getReader();
    const preventCancel = !!(options && options.preventCancel);
    let released = false;
    const release = () => { if (!released) { released = true; reader.releaseLock(); } };
    return {
      next() {
        if (released) return Promise.resolve({ value: undefined, done: true });
        return reader.read().then((r) => {
          if (r.done) release();
          return r;
        }, (e) => { release(); throw e; });
      },
      // `for await` calls this on normal completion too, by which point the
      // reader is already released -- cancelling it then would reject with
      // "reader has been released" and surface as an unhandled rejection.
      return(value) {
        if (!released) {
          if (!preventCancel) reader.cancel(value).catch(() => {});
          release();
        }
        return Promise.resolve({ value, done: true });
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };
  ReadableStream.prototype[Symbol.asyncIterator] = ReadableStream.prototype.values;
  ReadableStream.from = function (iterable) {
    const it = iterable[Symbol.asyncIterator]
      ? iterable[Symbol.asyncIterator]()
      : iterable[Symbol.iterator]();
    return new ReadableStream({
      async pull(c) {
        const r = await it.next();
        if (r.done) c.close(); else c.enqueue(r.value);
      },
      async cancel(reason) { if (typeof it.return === "function") await it.return(reason); },
    });
  };
  globalThis.ReadableStream = ReadableStream;

  function ReadableStreamDefaultReader(stream) {
    this._stream = stream;
    stream._reader = this;
    this._closedDeferred = deferred();
    if (stream._state === S_CLOSED) this._closedDeferred.resolve(undefined);
    else if (stream._state === S_ERRORED) this._closedDeferred.reject(stream._storedError);
    else stream._closedDeferred.promise.then(
      () => this._closedDeferred.resolve(undefined),
      (e) => this._closedDeferred.reject(e));
  }
  Object.defineProperty(ReadableStreamDefaultReader.prototype, "closed", {
    get() { return this._closedDeferred.promise; },
  });
  ReadableStreamDefaultReader.prototype.read = function () {
    const s = this._stream;
    if (!s) return Promise.reject(new TypeError("reader has been released"));
    s._disturbed = true;
    const c = s._controller;
    if (c._queue.length > 0) {
      const chunk = c._queue.shift();
      c._queueSize = Math.max(0, c._queueSize - 1);
      if (c._queue.length === 0 && c._closeRequested) readableClose(s);
      else controllerPull(c);
      return Promise.resolve({ value: chunk, done: false });
    }
    if (s._state === S_CLOSED) return Promise.resolve({ value: undefined, done: true });
    if (s._state === S_ERRORED) return Promise.reject(s._storedError);
    const d = deferred();
    s._readRequests.push(d);
    controllerPull(c);
    return d.promise;
  };
  ReadableStreamDefaultReader.prototype.cancel = function (reason) {
    const s = this._stream;
    if (!s) return Promise.reject(new TypeError("reader has been released"));
    return readableCancel(s, reason);
  };
  ReadableStreamDefaultReader.prototype.releaseLock = function () {
    const s = this._stream;
    if (!s) return;
    for (const r of s._readRequests.splice(0))
      r.reject(new TypeError("reader was released"));
    s._reader = null;
    this._stream = null;
  };
  globalThis.ReadableStreamDefaultReader = ReadableStreamDefaultReader;

  // ---- writable ----
  function WritableStreamDefaultController(stream, sink, strategy) {
    this._stream = stream;
    this._sink = sink;
    this._highWaterMark = strategy.highWaterMark === undefined ? 1 : Number(strategy.highWaterMark);
    this._sizeOf = typeof strategy.size === "function" ? strategy.size : () => 1;
    this.signal = typeof AbortController === "function" ? new AbortController().signal : undefined;
  }
  WritableStreamDefaultController.prototype.error = function (e) { writableError(this._stream, e); };

  function writableError(s, e) {
    if (s._state !== S_READABLE) return;
    s._state = S_ERRORED;
    s._storedError = e;
    s._closedDeferred.reject(e);
    for (const d of s._writeRequests.splice(0)) d.reject(e);
  }

  function WritableStream(sink, strategy) {
    sink = sink || {};
    strategy = strategy || {};
    this._state = S_READABLE;
    this._storedError = undefined;
    this._writer = null;
    this._queueSize = 0;
    this._writeRequests = [];
    this._closedDeferred = deferred();
    this._inFlight = Promise.resolve();
    const c = new WritableStreamDefaultController(this, sink, strategy);
    this._controller = c;
    this._startPromise = Promise.resolve().then(
      () => (typeof sink.start === "function" ? sink.start(c) : undefined));
    this._startPromise.catch((e) => writableError(this, e));
  }
  Object.defineProperty(WritableStream.prototype, "locked", {
    get() { return this._writer !== null; },
  });
  WritableStream.prototype.getWriter = function () {
    streamAssert(!this.locked, "stream is already locked");
    return new WritableStreamDefaultWriter(this);
  };
  WritableStream.prototype.abort = function (reason) {
    if (this.locked) return Promise.reject(new TypeError("stream is locked"));
    return writableAbort(this, reason);
  };
  WritableStream.prototype.close = function () {
    if (this.locked) return Promise.reject(new TypeError("stream is locked"));
    return writableClose(this);
  };
  function writableAbort(s, reason) {
    if (s._state !== S_READABLE) return Promise.resolve(undefined);
    const sink = s._controller._sink;
    writableError(s, reason === undefined ? new Error("aborted") : reason);
    return Promise.resolve(typeof sink.abort === "function" ? sink.abort(reason) : undefined)
      .then(() => undefined);
  }
  function writableClose(s) {
    if (s._state === S_ERRORED) return Promise.reject(s._storedError);
    if (s._state === S_CLOSED) return Promise.resolve(undefined);
    const sink = s._controller._sink;
    return s._inFlight
      .then(() => (typeof sink.close === "function" ? sink.close() : undefined))
      .then(() => {
        s._state = S_CLOSED;
        s._closedDeferred.resolve(undefined);
      }, (e) => { writableError(s, e); throw e; });
  }
  function writableWrite(s, chunk) {
    if (s._state === S_ERRORED) return Promise.reject(s._storedError);
    if (s._state === S_CLOSED) return Promise.reject(new TypeError("stream is closed"));
    const c = s._controller;
    let size = 1;
    try { size = c._sizeOf(chunk); } catch (e) { writableError(s, e); return Promise.reject(e); }
    s._queueSize += size;
    const run = s._inFlight
      .then(() => s._startPromise)
      .then(() => (typeof c._sink.write === "function" ? c._sink.write(chunk, c) : undefined))
      .then(() => { s._queueSize -= size; },
            (e) => { s._queueSize -= size; writableError(s, e); throw e; });
    // Keep the chain going even when this write rejects, so a later write
    // does not wait on a settled-rejected promise forever.
    s._inFlight = run.catch(() => {});
    return run;
  }
  globalThis.WritableStream = WritableStream;

  function WritableStreamDefaultWriter(stream) {
    this._stream = stream;
    stream._writer = this;
  }
  Object.defineProperty(WritableStreamDefaultWriter.prototype, "closed", {
    get() { return this._stream ? this._stream._closedDeferred.promise
                                : Promise.reject(new TypeError("writer was released")); },
  });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, "desiredSize", {
    get() {
      const s = this._stream;
      if (!s) throw new TypeError("writer was released");
      if (s._state === S_ERRORED) return null;
      if (s._state === S_CLOSED) return 0;
      return s._controller._highWaterMark - s._queueSize;
    },
  });
  Object.defineProperty(WritableStreamDefaultWriter.prototype, "ready", {
    get() {
      const s = this._stream;
      if (!s) return Promise.reject(new TypeError("writer was released"));
      if (s._state === S_ERRORED) return Promise.reject(s._storedError);
      // Backpressure: resolve once the queue is under the high-water mark.
      if (s._queueSize < s._controller._highWaterMark) return Promise.resolve(undefined);
      return s._inFlight.then(() => undefined);
    },
  });
  WritableStreamDefaultWriter.prototype.write = function (chunk) {
    const s = this._stream;
    if (!s) return Promise.reject(new TypeError("writer was released"));
    return writableWrite(s, chunk);
  };
  WritableStreamDefaultWriter.prototype.close = function () {
    const s = this._stream;
    if (!s) return Promise.reject(new TypeError("writer was released"));
    return writableClose(s);
  };
  WritableStreamDefaultWriter.prototype.abort = function (reason) {
    const s = this._stream;
    if (!s) return Promise.reject(new TypeError("writer was released"));
    return writableAbort(s, reason);
  };
  WritableStreamDefaultWriter.prototype.releaseLock = function () {
    if (this._stream) { this._stream._writer = null; this._stream = null; }
  };
  globalThis.WritableStreamDefaultWriter = WritableStreamDefaultWriter;

  // ---- transform ----
  function TransformStream(transformer, writableStrategy, readableStrategy) {
    transformer = transformer || {};
    let readableController;
    const controller = {
      enqueue: (chunk) => readableController.enqueue(chunk),
      error: (e) => { readableController.error(e); },
      terminate: () => { try { readableController.close(); } catch {} },
      get desiredSize() { return readableController.desiredSize; },
    };
    this.readable = new ReadableStream({
      start(c) { readableController = c; },
    }, readableStrategy || {});
    this.writable = new WritableStream({
      start() {
        return typeof transformer.start === "function" ? transformer.start(controller) : undefined;
      },
      write(chunk) {
        if (typeof transformer.transform === "function")
          return transformer.transform(chunk, controller);
        controller.enqueue(chunk);          // identity transform
        return undefined;
      },
      close() {
        return Promise.resolve(
          typeof transformer.flush === "function" ? transformer.flush(controller) : undefined
        ).then(() => { try { readableController.close(); } catch {} });
      },
      abort(reason) { readableController.error(reason); },
    }, writableStrategy || {});
  }
  globalThis.TransformStream = TransformStream;

  // ---- queuing strategies ----
  function CountQueuingStrategy(init) { this.highWaterMark = init.highWaterMark; }
  CountQueuingStrategy.prototype.size = function () { return 1; };
  globalThis.CountQueuingStrategy = CountQueuingStrategy;
  function ByteLengthQueuingStrategy(init) { this.highWaterMark = init.highWaterMark; }
  ByteLengthQueuingStrategy.prototype.size = function (chunk) { return chunk.byteLength; };
  globalThis.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;

  // ---- text transforms ----
  function TextEncoderStream() {
    const enc = new TextEncoder();
    TransformStream.call(this, {
      transform(chunk, c) { c.enqueue(enc.encode(String(chunk))); },
    });
    this.encoding = "utf-8";
  }
  globalThis.TextEncoderStream = TextEncoderStream;

  function TextDecoderStream(label, options) {
    const dec = new TextDecoder(label, options);
    TransformStream.call(this, {
      transform(chunk, c) {
        const text = dec.decode(chunk, { stream: true });
        if (text) c.enqueue(text);
      },
      flush(c) {
        const rest = dec.decode();
        if (rest) c.enqueue(rest);
      },
    });
    this.encoding = dec.encoding;
  }
  globalThis.TextDecoderStream = TextDecoderStream;

  // ---------------- File / FormData ----------------
  // File is a Blob with a name and a modified time; FormData is the multi-map
  // fetch bodies and form parsing are built on.
  function File(bits, name, options) {
    options = options || {};
    Blob.call(this, bits, options);
    this.name = String(name);
    this.lastModified = options.lastModified !== undefined ? options.lastModified : Date.now();
  }
  File.prototype = Object.create(Blob.prototype);
  File.prototype.constructor = File;
  globalThis.File = File;

  function FormData() { this._entries = []; }
  function fdNormalize(value, filename) {
    if (value instanceof File) return filename !== undefined
      ? new File([value], filename, { type: value.type }) : value;
    if (value instanceof Blob) return new File([value], filename === undefined ? "blob" : filename, { type: value.type });
    return String(value);
  }
  FormData.prototype.append = function (name, value, filename) {
    this._entries.push([String(name), fdNormalize(value, filename)]);
  };
  FormData.prototype.set = function (name, value, filename) {
    name = String(name);
    const v = fdNormalize(value, filename);
    let placed = false;
    this._entries = this._entries.filter(([k]) => {
      if (k !== name) return true;
      if (placed) return false;
      placed = true;
      return true;
    });
    const i = this._entries.findIndex(([k]) => k === name);
    if (i >= 0) this._entries[i] = [name, v]; else this._entries.push([name, v]);
  };
  FormData.prototype.get = function (name) {
    name = String(name);
    for (const [k, v] of this._entries) if (k === name) return v;
    return null;
  };
  FormData.prototype.getAll = function (name) {
    name = String(name);
    return this._entries.filter(([k]) => k === name).map(([, v]) => v);
  };
  FormData.prototype.has = function (name) {
    name = String(name);
    return this._entries.some(([k]) => k === name);
  };
  FormData.prototype.delete = function (name) {
    name = String(name);
    this._entries = this._entries.filter(([k]) => k !== name);
  };
  FormData.prototype.forEach = function (cb, thisArg) {
    for (const [k, v] of this._entries.slice()) cb.call(thisArg, v, k, this);
  };
  FormData.prototype.entries = function () { return this._entries.map((e) => e.slice())[Symbol.iterator](); };
  FormData.prototype.keys = function () { return this._entries.map(([k]) => k)[Symbol.iterator](); };
  FormData.prototype.values = function () { return this._entries.map(([, v]) => v)[Symbol.iterator](); };
  FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
  globalThis.FormData = FormData;

  // ---------------- structuredClone ----------------
  // A deep clone over the structured-clone graph: cycles preserved, the
  // built-in containers handled, functions and symbols rejected the way the
  // standard requires.
  globalThis.structuredClone = function structuredClone(value, options) {
    const seen = new Map();
    const walk = (v) => {
      if (v === null || typeof v !== "object") {
        if (typeof v === "function" || typeof v === "symbol")
          throw new DOMExceptionLike(String(v) + " could not be cloned.", "DataCloneError");
        return v;
      }
      if (seen.has(v)) return seen.get(v);
      let out;
      if (Array.isArray(v)) {
        out = []; seen.set(v, out);
        for (let i = 0; i < v.length; i++) out[i] = walk(v[i]);
        return out;
      }
      if (v instanceof Date) { out = new Date(v.getTime()); seen.set(v, out); return out; }
      if (v instanceof RegExp) { out = new RegExp(v.source, v.flags); seen.set(v, out); return out; }
      if (v instanceof Map) {
        out = new Map(); seen.set(v, out);
        for (const [k, val] of v) out.set(walk(k), walk(val));
        return out;
      }
      if (v instanceof Set) {
        out = new Set(); seen.set(v, out);
        for (const val of v) out.add(walk(val));
        return out;
      }
      if (v instanceof ArrayBuffer) { out = v.slice(0); seen.set(v, out); return out; }
      if (ArrayBuffer.isView(v)) {
        out = new v.constructor(walk(v.buffer), v.byteOffset, v.length !== undefined ? v.length : undefined);
        seen.set(v, out); return out;
      }
      if (v instanceof Error) {
        out = new v.constructor(v.message); seen.set(v, out);
        if (v.stack !== undefined) out.stack = v.stack;
        if (v.cause !== undefined) out.cause = walk(v.cause);
        return out;
      }
      if (typeof v === "function")
        throw new DOMExceptionLike("function could not be cloned.", "DataCloneError");
      out = {}; seen.set(v, out);
      for (const k of Object.keys(v)) out[k] = walk(v[k]);
      return out;
    };
    void options;
    return walk(value);
  };

  // A minimal DOMException stand-in: the name is what callers branch on.
  function DOMExceptionLike(message, name) {
    const e = new Error(message);
    e.name = name || "Error";
    return e;
  }
  if (typeof globalThis.DOMException === "undefined") {
    globalThis.DOMException = function DOMException(message, name) { return DOMExceptionLike(message, name); };
  }

  // ---------------- crypto interfaces, MessageChannel ----------------
  // The standard exposes these as constructors so `crypto instanceof Crypto`
  // and feature detection by name both work; the instances already exist.
  function Crypto() { throw new TypeError("Illegal constructor"); }
  function SubtleCrypto() { throw new TypeError("Illegal constructor"); }
  function CryptoKey() { throw new TypeError("Illegal constructor"); }
  globalThis.Crypto = Crypto;
  globalThis.SubtleCrypto = SubtleCrypto;
  globalThis.CryptoKey = CryptoKey;

  // An in-process message channel: both ports are EventTargets and a post on
  // one is delivered asynchronously to the other, which is the contract code
  // written against it relies on.
  function MessagePort() {
    EventTarget.call(this);
    this._other = null;
    this._started = false;
    this._pending = [];
    this.onmessage = null;
  }
  MessagePort.prototype = Object.create(EventTarget.prototype);
  MessagePort.prototype.constructor = MessagePort;
  MessagePort.prototype.postMessage = function (data) {
    const other = this._other;
    if (!other) return;
    const deliver = () => {
      const ev = new MessageEvent("message", { data: structuredClone(data) });
      if (typeof other.onmessage === "function") other.onmessage.call(other, ev);
      other.dispatchEvent(ev);
    };
    // A port that has not been started buffers, exactly as the spec says.
    if (other._started || typeof other.onmessage === "function") queueMicrotask(deliver);
    else other._pending.push(deliver);
  };
  MessagePort.prototype.start = function () {
    if (this._started) return;
    this._started = true;
    const queued = this._pending.splice(0);
    for (const d of queued) queueMicrotask(d);
  };
  MessagePort.prototype.close = function () { this._other = null; };
  globalThis.MessagePort = MessagePort;

  function MessageEvent(type, init) {
    Event.call(this, type, init);
    init = init || {};
    this.data = init.data;
    this.origin = init.origin || "";
    this.lastEventId = init.lastEventId || "";
    this.source = init.source || null;
    this.ports = init.ports || [];
  }
  MessageEvent.prototype = Object.create(Event.prototype);
  MessageEvent.prototype.constructor = MessageEvent;
  globalThis.MessageEvent = MessageEvent;

  function MessageChannel() {
    this.port1 = new MessagePort();
    this.port2 = new MessagePort();
    this.port1._other = this.port2;
    this.port2._other = this.port1;
  }
  globalThis.MessageChannel = MessageChannel;

  // ---------------- navigator ----------------
  if (typeof globalThis.navigator === "undefined") {
    globalThis.navigator = Object.freeze({
      userAgent: "sxn/" + (Sxn && Sxn.version ? Sxn.version : "0"),
      // WinterCG names this for runtime detection.
      platform: "",
    });
  }

  // ---------------- crypto ----------------
  globalThis.crypto = {
    // RFC 4122 v4 from the same CSPRNG getRandomValues uses.
    randomUUID: function () {
      const b = new Uint8Array(16);
      globalThis.crypto.getRandomValues(b);
      b[6] = (b[6] & 0x0f) | 0x40;   // version 4
      b[8] = (b[8] & 0x3f) | 0x80;   // variant 10xx
      const h = [];
      for (let i = 0; i < 16; i++) h.push(b[i].toString(16).padStart(2, "0"));
      return h.slice(0,4).join("") + "-" + h.slice(4,6).join("") + "-" +
             h.slice(6,8).join("") + "-" + h.slice(8,10).join("") + "-" + h.slice(10,16).join("");
    },
    getRandomValues: function (typedArray) {
      if (!ArrayBuffer.isView(typedArray)) throw new TypeError("crypto.getRandomValues expects an integer typed array");
      var bytes = __sxnRandomBytes(typedArray.byteLength);
      new Uint8Array(typedArray.buffer, typedArray.byteOffset, typedArray.byteLength).set(bytes);
      return typedArray;
    },
    subtle: {
      digest: async function (algorithm, data) {
        var name = typeof algorithm === "string" ? algorithm : algorithm.name;
        var bytes;
        if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
        else if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        else throw new TypeError("crypto.subtle.digest expects a BufferSource");
        return __sxnDigest(name, bytes).buffer;
      }
    }
  };

  // Brand the instances now that they exist, so `crypto instanceof Crypto`
  // holds the way the standard describes.
  Object.setPrototypeOf(globalThis.crypto, Crypto.prototype);
  if (globalThis.crypto.subtle) Object.setPrototypeOf(globalThis.crypto.subtle, SubtleCrypto.prototype);
})();
