/* WinterCG-ish globals layered on top of the native bindings installed by
   sxn_install_network (the __sxn* functions below). Pure parsing/spec logic
   lives here in JS; anything needing tight C integration (the streaming
   fetch body, random bytes, digests, the monotonic clock) is a thin native
   primitive that this file wraps in a spec-shaped class. */
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
    for (var i = 0; i < this._list.length; i++) {
      if (this._list[i][0] === name) { this._list[i][1] += ", " + value; return; }
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
  globalThis.Headers = Headers;

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
  function singleChunkStream(bytes) {
    var done = false;
    return {
      getReader: function () {
        return {
          read: function () {
            if (done) return Promise.resolve({ value: undefined, done: true });
            done = true;
            return Promise.resolve({ value: bytes, done: false });
          },
          cancel: function () { done = true; return Promise.resolve(); }
        };
      }
    };
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
      if (this._rawStream) return this._rawStream;
      if (this._staticBytes !== undefined) return singleChunkStream(this._staticBytes);
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

  // ---------------- crypto ----------------
  globalThis.crypto = {
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
})();
