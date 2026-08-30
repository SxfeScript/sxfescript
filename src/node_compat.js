/* Minimal `node:*` compatibility layer, layered the same way bootstrap.js
   layers WinterCG globals: pure spec/behavior logic lives here in JS, native
   primitives (env, cwd, exit, signal watching) are thin C wrappers installed
   by sxn_install_node_compat before this file is evaluated. The four
   `node:*` C modules (see src/node.c) just re-export the globals this file
   defines. */
(function () {
  // ---------------- events: EventEmitter ----------------
  // on/off/emit/listenerCount/listeners/removeAllListeners are native (see
  // js_ee_* in src/node.c, registered as globals above) -- phase 3 of
  // replacing node_compat.js with native C. Listener storage is still the
  // plain `this._events` object with Node's function-for-one-listener and
  // array-for-many representation; the native side reads and writes it
  // directly. `once` stays JS: its self-removing wrapper has no natural
  // native shape (it needs to reference itself to call `self.off(type,
  // wrapped)`), and it's not a hot path the way emit() is.
  function EventEmitter() {
    this._events = Object.create(null);
  }
  EventEmitter.prototype.on = __sxnEeOn;
  EventEmitter.prototype.addListener = __sxnEeOn;
  EventEmitter.prototype.once = function (type, listener) {
    var self = this;
    function wrapped() {
      self.off(type, wrapped);
      listener.apply(this, arguments);
    }
    wrapped._original = listener;
    return this.on(type, wrapped);
  };
  EventEmitter.prototype.off = __sxnEeOff;
  EventEmitter.prototype.removeListener = __sxnEeOff;
  EventEmitter.prototype.removeAllListeners = __sxnEeRemoveAllListeners;
  EventEmitter.prototype.emit = __sxnEeEmit;
  EventEmitter.prototype.listenerCount = __sxnEeListenerCount;
  EventEmitter.prototype.listeners = __sxnEeListeners;
  // events.once(emitter, name): resolves with the emitted arguments, or
  // rejects if the emitter emits 'error' first. Widely used to await a
  // one-shot lifecycle event such as 'listening'.
  EventEmitter.once = function (emitter, name, options) {
    return new Promise((resolve, reject) => {
      const onEvent = (...args) => { cleanup(); resolve(args); };
      const onError = (err) => { cleanup(); reject(err); };
      const cleanup = () => {
        emitter.off(name, onEvent);
        if (name !== "error") emitter.off("error", onError);
        if (signal) signal.removeEventListener("abort", onAbort);
      };
      const signal = options && options.signal;
      const onAbort = () => { cleanup(); reject(signal.reason || new Error("aborted")); };
      emitter.on(name, onEvent);
      if (name !== "error") emitter.on("error", onError);
      if (signal) {
        if (signal.aborted) return onAbort();
        signal.addEventListener("abort", onAbort);
      }
    });
  };
  EventEmitter.on = function (emitter, name) {
    const queue = [], waiting = [];
    emitter.on(name, (...args) => {
      if (waiting.length) waiting.shift()({ value: args, done: false });
      else queue.push(args);
    });
    return {
      next() {
        if (queue.length) return Promise.resolve({ value: queue.shift(), done: false });
        return new Promise((res) => waiting.push(res));
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };
  globalThis.__sxnEventEmitter = EventEmitter;
  delete globalThis.__sxnEeOn;
  delete globalThis.__sxnEeOff;
  delete globalThis.__sxnEeEmit;
  delete globalThis.__sxnEeListenerCount;
  delete globalThis.__sxnEeListeners;
  delete globalThis.__sxnEeRemoveAllListeners;

  // ---------------- buffer: Buffer ----------------
  // Only called for non-utf-8 encodings -- Buffer.from's string branch
  // handles utf-8 itself via a native primitive that skips the extra
  // allocation this returns-a-bare-Uint8Array shape would otherwise cost.
  // `encoding` arrives already-lowercased from that call site.
  function bufferBytesFromString(str, encoding) {
    if (encoding === "hex") return Uint8Array.fromHex(str);
    if (encoding === "base64") return Uint8Array.fromBase64(str);
    if (encoding === "base64url") return Uint8Array.fromBase64(str, { alphabet: "base64url" });
    if (encoding === "latin1" || encoding === "binary" || encoding === "ascii") {
      var bytes = new Uint8Array(str.length);
      for (var i = 0; i < str.length; i++) bytes[i] = str.charCodeAt(i) & 0xff;
      return bytes;
    }
    throw new TypeError("Unknown encoding: " + encoding);
  }

  class Buffer extends Uint8Array {
    toString(encoding) {
      // Fast path: match the exact lowercase spelling (and the implicit
      // default) every real call site actually passes, with plain ===
      // comparisons -- measured ~2x faster than always paying for
      // String(encoding||"utf-8").toLowerCase()'s per-call string
      // allocation before even checking what the encoding is.
      if (encoding === undefined || encoding === "utf-8" || encoding === "utf8") return __sxnUtf8Decode(this, false, false).text; // skip a TextDecoder allocation per call
      if (encoding === "hex") return this.toHex();
      if (encoding === "base64") return this.toBase64();
      if (encoding === "base64url") return this.toBase64({ alphabet: "base64url", omitPadding: true }); // Node emits base64url unpadded
      encoding = String(encoding).toLowerCase();
      if (encoding === "utf-8" || encoding === "utf8") return __sxnUtf8Decode(this, false, false).text;
      if (encoding === "hex") return this.toHex();
      if (encoding === "base64") return this.toBase64();
      if (encoding === "base64url") return this.toBase64({ alphabet: "base64url", omitPadding: true }); // Node emits base64url unpadded
      if (encoding === "latin1" || encoding === "binary") {
        var out = "";
        for (var i = 0; i < this.length; i++) out += String.fromCharCode(this[i]);
        return out;
      }
      if (encoding === "ascii") {
        // Node's "ascii" is 7-bit: the high bit is stripped, unlike latin1.
        var a = "";
        for (var j = 0; j < this.length; j++) a += String.fromCharCode(this[j] & 0x7f);
        return a;
      }
      throw new TypeError("Unknown encoding: " + encoding);
    }
    // Node's Buffer#slice (and #subarray) are zero-copy views over the same
    // backing ArrayBuffer, unlike TypedArray#slice (which copies) -- alias
    // it to #subarray to get Node's semantics instead of the inherited one.
    slice(start, end) {
      return this.subarray(start, end);
    }
    // alloc/allocUnsafe/isBuffer are installed natively after this class is
    // defined (see sxn_install_buffer_natives in src/node.c) -- pure
    // dispatch with no encoding logic, so there's nothing JS needs to do
    // here.
    static from(data, encoding) {
      if (typeof data === "string") {
        // Fast path: exact-lowercase-spelling === checks (matching every
        // real call site, including the implicit default) skip the
        // String(encoding||"utf-8").toLowerCase() allocation below entirely.
        //
        // utf-8 goes through a native primitive that hands back a bare
        // ArrayBuffer, so only one JS-visible allocation (the Buffer itself)
        // happens instead of two (a throwaway Uint8Array view plus the
        // Buffer wrapping its .buffer/.byteOffset/.byteLength). Other
        // encodings already produce a single typed array (via
        // Uint8Array.fromHex/fromBase64 or the latin1 loop); reparenting it
        // to Buffer.prototype in place is measurably faster here than
        // constructing a second Buffer instance around its .buffer.
        // Node ignores a non-string encoding rather than coercing it, so an
        // object with a toString is treated as absent and never called.
        if (typeof encoding !== "string") encoding = undefined;
        if (encoding === undefined || encoding === "utf-8" || encoding === "utf8") return new Buffer(__sxnUtf8EncodeArrayBuffer(data));
        if (encoding === "hex" || encoding === "base64" || encoding === "base64url") {
          return Object.setPrototypeOf(bufferBytesFromString(data, encoding), Buffer.prototype);
        }
        var enc = String(encoding).toLowerCase();
        if (enc === "utf-8" || enc === "utf8") return new Buffer(__sxnUtf8EncodeArrayBuffer(data));
        return Object.setPrototypeOf(bufferBytesFromString(data, enc), Buffer.prototype);
      }
      if (data instanceof ArrayBuffer) return new Buffer(data); // zero-copy view over the whole buffer
      if (ArrayBuffer.isView(data)) return new Buffer(data.buffer, data.byteOffset, data.byteLength); // zero-copy view
      if (Array.isArray(data) || (data && typeof data.length === "number")) return new Buffer(data); // copies, matching Node
      throw new TypeError("Buffer.from: unsupported argument");
    }
    // Node serializes a Buffer as { type: "Buffer", data: [...] }, and code
    // round-trips through that shape.
    toJSON() { return { type: "Buffer", data: Array.from(this) }; }

    static concat(list, totalLength) {
      if (totalLength === undefined) {
        totalLength = 0;
        for (var i = 0; i < list.length; i++) totalLength += list[i].length;
      }
      var out = new Buffer(totalLength);
      var offset = 0;
      for (var j = 0; j < list.length && offset < totalLength; j++) {
        var chunk = list[j].subarray(0, Math.min(list[j].length, totalLength - offset));
        out.set(chunk, offset);
        offset += chunk.length;
      }
      return out;
    }
  }
  globalThis.Buffer = Buffer;

  // ---------------- path: posix / win32 ----------------
  // Reduces a split path into a normalized segment list: drops "." and
  // empty segments, resolves ".." against the previous real segment, and
  // (for relative paths) keeps a leading run of ".." since there's no root
  // to clamp against.
  function reduceSegments(parts, isAbsolutePath) {
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var seg = parts[i];
      if (seg === "" || seg === ".") continue;
      if (seg === "..") {
        if (out.length && out[out.length - 1] !== "..") out.pop();
        else if (!isAbsolutePath) out.push("..");
      } else {
        out.push(seg);
      }
    }
    return out;
  }

  function makePathImpl(sep, delimiter, splitRe, isAbsoluteFn, formatRoot) {
    function normalize(p) {
      p = String(p);
      if (p === "") return ".";
      var isAbs = isAbsoluteFn(p);
      var root = formatRoot(p, isAbs);
      var rest = p.slice(root.rootLength);
      var trailingSep = rest.length > 0 && splitRe.test(rest.charAt(rest.length - 1));
      var segments = reduceSegments(rest.split(splitRe), isAbs);
      var out = root.prefix + segments.join(sep);
      if (out === "") out = ".";
      if (trailingSep && segments.length && out.charAt(out.length - 1) !== sep) out += sep;
      return out;
    }

    function join() {
      var parts = [];
      for (var i = 0; i < arguments.length; i++) {
        var a = arguments[i];
        if (a === undefined || a === null) continue;
        if (typeof a !== "string") throw new TypeError("path segments must be strings");
        if (a.length) parts.push(a);
      }
      if (!parts.length) return ".";
      return normalize(parts.join(sep));
    }

    function resolve() {
      var resolved = "";
      var resolvedAbsolute = false;
      for (var i = arguments.length - 1; i >= -1 && !resolvedAbsolute; i--) {
        var seg = i >= 0 ? arguments[i] : __sxnCwd();
        if (!seg) continue;
        resolved = seg + sep + resolved;
        resolvedAbsolute = isAbsoluteFn(seg);
      }
      var out = normalize(resolved);
      if (!resolvedAbsolute) out = normalize(__sxnCwd() + sep + resolved);
      // strip a normalize()-added trailing separator, resolve() never keeps one
      var root = formatRoot(out, true);
      if (out.length > root.rootLength && splitRe.test(out.charAt(out.length - 1))) out = out.slice(0, -1);
      return out;
    }

    function dirname(p) {
      p = String(p);
      var isAbs = isAbsoluteFn(p);
      var root = formatRoot(p, isAbs);
      var rest = p.slice(root.rootLength);
      var end = -1, matchedSep = true;
      for (var i = rest.length - 1; i >= 0; i--) {
        if (splitRe.test(rest.charAt(i))) {
          if (!matchedSep) { end = i; break; }
        } else matchedSep = false;
      }
      if (end === -1) return root.rootLength ? (root.prefix || root.rootPath) : ".";
      return root.prefix + rest.slice(0, end);
    }

    function basename(p, suffix) {
      p = String(p);
      var root = formatRoot(p, isAbsoluteFn(p));
      var rest = p.slice(root.rootLength).replace(new RegExp("[" + (sep === "\\" ? "\\\\/" : "/") + "]+$"), "");
      var idx = -1;
      for (var i = rest.length - 1; i >= 0; i--) { if (splitRe.test(rest.charAt(i))) { idx = i; break; } }
      var base = idx === -1 ? rest : rest.slice(idx + 1);
      if (suffix && base.length > suffix.length && base.slice(-suffix.length) === suffix) {
        base = base.slice(0, -suffix.length);
      }
      return base;
    }

    function extname(p) {
      var base = basename(p);
      var dot = base.lastIndexOf(".");
      if (dot <= 0) return ""; // no dot, or a leading dot (e.g. ".gitignore")
      return base.slice(dot);
    }

    function relative(from, to) {
      from = resolve(from);
      to = resolve(to);
      if (from === to) return "";
      var fromParts = from.split(sep).filter(Boolean);
      var toParts = to.split(sep).filter(Boolean);
      var common = 0;
      while (common < fromParts.length && common < toParts.length &&
             (sep === "\\" ? fromParts[common].toLowerCase() === toParts[common].toLowerCase() : fromParts[common] === toParts[common])) {
        common++;
      }
      var ups = fromParts.length - common;
      var out = [];
      for (var i = 0; i < ups; i++) out.push("..");
      return out.concat(toParts.slice(common)).join(sep);
    }

    return {
      sep: sep,
      delimiter: delimiter,
      isAbsolute: function (p) { return isAbsoluteFn(String(p)); },
      normalize: normalize,
      join: join,
      resolve: resolve,
      dirname: dirname,
      basename: basename,
      extname: extname,
      relative: relative,
    };
  }

  // Native (see js_path_posix_* in src/node.c, registered as globals above)
  // -- phase 4 of replacing node_compat.js with native C. Same
  // reduceSegments/makePathImpl algorithm, ported to walk C strings
  // instead of JS regex splits; win32 below is unchanged, still the
  // generic JS implementation.
  var posix = {
    sep: "/",
    delimiter: ":",
    isAbsolute: __sxnPosixIsAbsolute,
    normalize: __sxnPosixNormalize,
    join: __sxnPosixJoin,
    resolve: __sxnPosixResolve,
    dirname: __sxnPosixDirname,
    basename: __sxnPosixBasename,
    extname: __sxnPosixExtname,
    relative: __sxnPosixRelative,
  };

  var WIN32_SPLIT_RE = /[\\/]/;
  function win32Root(p) {
    // UNC: \\server\share\...
    var unc = /^[\\/]{2}[^\\/]+[\\/]+[^\\/]+/.exec(p);
    if (unc) return { rootLength: unc[0].length, prefix: unc[0].replace(/[\\/]+$/, "\\") + "\\", rootPath: unc[0] };
    // Drive-qualified: C:\... or C:...
    var drive = /^[a-zA-Z]:[\\/]?/.exec(p);
    if (drive) {
      var withSep = /[\\/]$/.test(drive[0]);
      return { rootLength: drive[0].length, prefix: drive[0].slice(0, 2) + (withSep ? "\\" : ""), rootPath: drive[0].slice(0, 2) + "\\" };
    }
    if (WIN32_SPLIT_RE.test(p.charAt(0))) return { rootLength: 1, prefix: "\\", rootPath: "\\" };
    return { rootLength: 0, prefix: "", rootPath: "" };
  }
  function win32IsAbsolute(p) {
    if (/^[\\/]{2}/.test(p)) return true; // UNC: \\server\share
    if (/^[a-zA-Z]:[\\/]/.test(p)) return true; // drive-qualified: C:\...
    if (/^[\\/]/.test(p)) return true; // drive-relative: \foo
    return false;
  }
  var win32 = makePathImpl("\\", ";", WIN32_SPLIT_RE, win32IsAbsolute, win32Root);

  var path = __sxnIsWindows ? win32 : posix;
  path.posix = posix;
  path.win32 = win32;
  globalThis.__sxnPath = path;
  delete globalThis.__sxnPosixJoin;
  delete globalThis.__sxnPosixResolve;
  delete globalThis.__sxnPosixNormalize;
  delete globalThis.__sxnPosixIsAbsolute;
  delete globalThis.__sxnPosixDirname;
  delete globalThis.__sxnPosixBasename;
  delete globalThis.__sxnPosixExtname;
  delete globalThis.__sxnPosixRelative;

  // ---------------- process ----------------
  function Process() {
    EventEmitter.call(this);
  }
  Process.prototype = Object.create(EventEmitter.prototype);
  Process.prototype.constructor = Process;

  var process = new Process();
  process.argv = [__sxnExecPath].concat(typeof scriptArgs !== "undefined" ? scriptArgs : []);
  // Native exotic object (see sxn_new_env_object in src/node.c) -- getenv/
  // setenv/unsetenv/environ directly behind property access, no Proxy trap
  // round-trip through JS for every read/write.
  process.env = globalThis.__sxnEnvObject;
  delete globalThis.__sxnEnvObject;
  // Node names these, and packages branch on them for path separators, line
  // endings and native-binary selection.
  process.platform = __sxnPlatform();
  process.arch = __sxnArch();
  process.version = "v" + (Sxn && Sxn.version ? Sxn.version : "0.0.0");
  process.versions = { sxn: (Sxn && Sxn.version) || "0.0.0" };
  // process.stdout / stderr: Writable enough for the logging that packages do
  // on the way past, and carrying the fd and isTTY they branch on.
  function makeStdio(fd, sink) {
    return {
      fd: fd,
      // Node leaves this undefined rather than false on a non-tty.
      isTTY: undefined,
      writable: true,
      columns: undefined,
      rows: undefined,
      write(chunk, enc, cb) {
        if (typeof enc === "function") { cb = enc; }
        sink(typeof chunk === "string" ? chunk : new TextDecoder().decode(chunk));
        if (cb) cb();
        return true;
      },
      end(chunk) { if (chunk !== undefined && chunk !== null) this.write(chunk); },
      on() { return this; },
      once() { return this; },
      off() { return this; },
      removeListener() { return this; },
      emit() { return false; },
      setEncoding() { return this; },
      cork() {}, uncork() {},
    };
  }
  // console.log appends its own newline, so the raw writes trim one to avoid
  // doubling it.
  const stripNL = (s) => (s.endsWith("\n") ? s.slice(0, -1) : s);
  process.stdout = makeStdio(1, (t) => { if (t) console.log(stripNL(t)); });
  process.stderr = makeStdio(2, (t) => { if (t) console.error(stripNL(t)); });
  process.stdin = { fd: 0, isTTY: false, readable: false,
                    on() { return this; }, once() { return this; }, off() { return this; },
                    resume() { return this; }, pause() { return this; },
                    setEncoding() { return this; }, read() { return null; } };
  process.hrtime = Object.assign(
    function hrtime(prev) {
      const ns = BigInt(Math.round(performance.now() * 1e6));
      const s = Number(ns / 1000000000n), n = Number(ns % 1000000000n);
      if (!prev) return [s, n];
      let ds = s - prev[0], dn = n - prev[1];
      if (dn < 0) { ds -= 1; dn += 1e9; }
      return [ds, dn];
    },
    { bigint: () => BigInt(Math.round(performance.now() * 1e6)) });
  process.emitWarning = function (w) { console.error("Warning: " + (w && w.message ? w.message : w)); };
  process.uptime = function () { return performance.now() / 1000; };
  process.pid = 0;
  process.cwd = function () { return __sxnCwd(); };
  process.exit = function (code) { __sxnExit(code === undefined ? 0 : code); };
  // A genuine job-queue microtask (queueMicrotask is itself a thin JS_EnqueueJob
  // wrapper built into quickjs.c), not a timer -- so nextTick callbacks always
  // run before any subsequently-scheduled timer/I-O callback, matching Node's
  // "runs before the event loop continues" contract for the cases this runtime
  // supports.
  process.nextTick = function (fn) {
    var args = Array.prototype.slice.call(arguments, 1);
    queueMicrotask(function () { fn.apply(undefined, args); });
  };

  // process.on('SIGINT'/'SIGTERM', ...) only arms the native libuv signal
  // watcher (see __sxnWatchSignal in src/node.c) the first time a listener
  // is added for that signal, matching Node's semantics of leaving default
  // OS signal disposition alone until the script opts in.
  var armedSignals = Object.create(null);
  var baseOn = EventEmitter.prototype.on;
  process.on = function (type, listener) {
    if ((type === "SIGINT" || type === "SIGTERM") && !armedSignals[type]) {
      armedSignals[type] = true;
      __sxnWatchSignal(type, function () { process.emit(type); });
    }
    return baseOn.call(this, type, listener);
  };
  process.addListener = process.on;

  globalThis.process = process;

  // ---------------- fs / fs/promises ----------------
  // writeFileSync/existsSync/writeFile are pure passthroughs with no logic
  // of their own, so they're bound directly to native exports (src/node.c's
  // __sxnWriteFileSync/__sxnExistsSync, src/network.c's __sxnWriteFileAsync)
  // -- phase 2 of replacing node_compat.js with native C. readFileSync/
  // readFile still branch on encoding and construct a Buffer, so they stay
  // JS until Buffer itself goes native. Encoding support is limited to
  // utf-8 (the only encoding TextDecoder/TextEncoder in bootstrap.js
  // support); without an encoding argument, reads return a Buffer, but
  // bytes still round-trip through UTF-8 decode/encode -- exact for text
  // content and lossy only for non-UTF-8 binary content.
  function wantsText(encoding) {
    return typeof encoding === "string" || (encoding && typeof encoding.encoding === "string");
  }
  var fs = {
    readFileSync: function (path, encoding) {
      var bytes = __sxnReadFileSync(path);
      if (wantsText(encoding)) return new TextDecoder().decode(bytes);
      return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    },
    writeFileSync: globalThis.__sxnWriteFileSync,
    existsSync: globalThis.__sxnExistsSync,
  };
  globalThis.__sxnFs = fs;
  delete globalThis.__sxnWriteFileSync;
  delete globalThis.__sxnExistsSync;

  var fsPromises = {
    readFile: function (path, encoding) {
      return Sxn.file(path).text().then(function (text) {
        return wantsText(encoding) ? text : Buffer.from(new TextEncoder().encode(text).buffer);
      });
    },
    writeFile: __sxnWriteFileAsync,
  };
  globalThis.__sxnFsPromises = fsPromises;


  // ---------------- node:stream ----------------
  // Node's EventEmitter-shaped streams, on top of the same EventEmitter this
  // runtime already ships. Flowing mode ('data' listeners and resume) and
  // paused mode (read()) both work, as do pipe, backpressure through the
  // write callback, and the Web Streams bridges.
  const EE = globalThis.__sxnEventEmitter;

  function inheritEE(Ctor) {
    Ctor.prototype = Object.create(EE.prototype);
    Ctor.prototype.constructor = Ctor;
  }

  function Readable(options) {
    EE.call(this);
    options = options || {};
    this._buf = [];
    this._flowing = false;
    this._ended = false;
    this._endEmitted = false;
    this._destroyed = false;
    this.readable = true;
    this._readableObjectMode = !!options.objectMode;
    this._encoding = options.encoding || null;
    if (typeof options.read === "function") this._read = options.read;
    if (typeof options.destroy === "function") this._destroyImpl = options.destroy;
  }
  inheritEE(Readable);
  Readable.prototype._read = function () {};
  Readable.prototype.push = function (chunk) {
    if (chunk !== null && badChunk(this, chunk)) throw chunkTypeError(chunk);
    if (chunk === null) {
      this._ended = true;
      maybeEndReadable(this);
      return false;
    }
    if (this._encoding && chunk instanceof Uint8Array) {
      chunk = new TextDecoder(this._encoding).decode(chunk);
    } else if (!this._readableObjectMode) {
      // Outside objectMode a stream carries bytes, so a string becomes a
      // Buffer -- consumers call chunk.toString() and expect one.
      if (typeof chunk === "string") chunk = Buffer.from(chunk, "utf-8");
      else if (chunk instanceof ArrayBuffer) chunk = Buffer.from(chunk);
      else if (chunk instanceof Uint8Array && !Buffer.isBuffer(chunk))
        chunk = Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    }
    this._buf.push(chunk);
    if (this._flowing) drainReadable(this);
    else this.emit("readable");
    return this._buf.length < 16;      // a coarse high-water mark
  };
  function drainReadable(r) {
    while (r._flowing && r._buf.length > 0) r.emit("data", r._buf.shift());
    maybeEndReadable(r);
    if (r._flowing && !r._ended) r._read();
  }
  function maybeEndReadable(r) {
    if (r._ended && r._buf.length === 0 && !r._endEmitted) {
      r._endEmitted = true;
      r.readable = false;
      queueMicrotask(() => { r.emit("end"); r.emit("close"); });
    }
  }
  Readable.prototype.read = function () {
    if (this._buf.length === 0) { this._read(); }
    if (this._buf.length === 0) { maybeEndReadable(this); return null; }
    let c;
    if (this._readableObjectMode || this._encoding || this._buf.length === 1) {
      c = this._buf.shift();
    } else {
      // Node hands back everything buffered as one Buffer.
      c = Buffer.concat(this._buf.splice(0));
    }
    maybeEndReadable(this);
    return c;
  };
  Readable.prototype.pause = function () { this._flowing = false; return this; };
  Readable.prototype.resume = function () {
    if (!this._flowing) { this._flowing = true; queueMicrotask(() => drainReadable(this)); }
    return this;
  };
  Readable.prototype.setEncoding = function (enc) { this._encoding = enc; return this; };
  Readable.prototype.destroy = function (err) {
    if (this._destroyed) return this;
    this._destroyed = true;
    this.readable = false;
    if (this._destroyImpl) this._destroyImpl(err || null, () => {});
    queueMicrotask(() => { if (err) this.emit("error", err); this.emit("close"); });
    return this;
  };
  Readable.prototype.pipe = function (dest, options) {
    this.on("data", (chunk) => { if (dest.write(chunk) === false) this.pause(); });
    dest.on("drain", () => this.resume());
    this.on("end", () => { if (!options || options.end !== false) dest.end(); });
    this.on("error", (e) => dest.emit("error", e));
    this.resume();
    return dest;
  };
  // `on('data')` is what switches a stream into flowing mode in Node.
  const readableOn = Readable.prototype.on;
  Readable.prototype.on = function (event, fn) {
    const r = EE.prototype.on.call(this, event, fn);
    if (event === "data") this.resume();
    return r;
  };
  void readableOn;
  Readable.prototype[Symbol.asyncIterator] = function () {
    const self = this;
    return {
      next() {
        if (self._buf.length > 0) return Promise.resolve({ value: self._buf.shift(), done: false });
        if (self._ended) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve, reject) => {
          const onData = (c) => { cleanup(); resolve({ value: c, done: false }); };
          const onEnd = () => { cleanup(); resolve({ value: undefined, done: true }); };
          const onErr = (e) => { cleanup(); reject(e); };
          const cleanup = () => {
            self.off("data", onData); self.off("end", onEnd); self.off("error", onErr);
          };
          self.on("data", onData); self.on("end", onEnd); self.on("error", onErr);
        });
      },
      [Symbol.asyncIterator]() { return this; },
    };
  };
  Readable.from = function (iterable, options) {
    const it = iterable[Symbol.asyncIterator]
      ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
    const r = new Readable(Object.assign({ objectMode: true }, options));
    let pulling = false;
    r._read = function () {
      if (pulling) return;
      pulling = true;
      Promise.resolve(it.next()).then(({ value, done }) => {
        pulling = false;
        if (done) r.push(null); else r.push(value);
      }, (e) => { pulling = false; r.destroy(e); });
    };
    return r;
  };
  Readable.fromWeb = function (webStream, options) {
    const reader = webStream.getReader();
    const r = new Readable(options);
    let pulling = false;
    r._read = function () {
      if (pulling) return;
      pulling = true;
      reader.read().then(({ value, done }) => {
        pulling = false;
        if (done) r.push(null); else r.push(value);
      }, (e) => { pulling = false; r.destroy(e); });
    };
    return r;
  };
  Readable.toWeb = function (nodeStream) {
    return new ReadableStream({
      start(c) {
        nodeStream.on("data", (chunk) => c.enqueue(chunk));
        nodeStream.on("end", () => { try { c.close(); } catch {} });
        nodeStream.on("error", (e) => c.error(e));
      },
      cancel() { nodeStream.destroy(); },
    });
  };

  function Writable(options) {
    EE.call(this);
    options = options || {};
    this._writableObjectMode = !!options.objectMode;
    this.writable = true;
    this._destroyed = false;
    this._finished = false;
    this._corked = 0;
    if (typeof options.write === "function") this._write = options.write;
    if (typeof options.final === "function") this._final = options.final;
    if (typeof options.destroy === "function") this._destroyImpl = options.destroy;
  }
  inheritEE(Writable);
  Writable.prototype._write = function (chunk, enc, cb) { cb(); };
  function chunkTypeError(chunk) {
    const e = new TypeError(
      'The "chunk" argument must be of type string or an instance of Buffer, ' +
      "TypedArray, or DataView. Received type " + typeof chunk + " (" + String(chunk) + ")");
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  }
  function badChunk(stream, chunk) {
    // Outside objectMode a stream carries bytes, and Node refuses anything
    // else rather than stringifying it silently.
    if (stream._writableObjectMode || stream._readableObjectMode) return false;
    return !(typeof chunk === "string" || chunk instanceof Uint8Array ||
             chunk instanceof ArrayBuffer || ArrayBuffer.isView(chunk));
  }
  Writable.prototype.write = function (chunk, enc, cb) {
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (badChunk(this, chunk)) throw chunkTypeError(chunk);
    if (!this.writable) {
      const e = new Error("write after end");
      queueMicrotask(() => this.emit("error", e));
      if (cb) cb(e);
      return false;
    }
    let sync = true, backpressure = false;
    this._write(chunk, enc || "utf8", (err) => {
      if (err) { this.emit("error", err); if (cb) cb(err); return; }
      if (cb) cb(null);
      if (backpressure) queueMicrotask(() => this.emit("drain"));
    });
    sync = false; void sync;
    return !backpressure;
  };
  Writable.prototype.cork = function () { this._corked++; };
  Writable.prototype.uncork = function () { if (this._corked) this._corked--; };
  Writable.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, enc);
    if (!this.writable) { if (cb) cb(); return this; }
    this.writable = false;
    const done = () => {
      this._finished = true;
      if (cb) cb();
      this.emit("finish");
      this.emit("close");
    };
    if (this._final) this._final((err) => { if (err) this.emit("error", err); else done(); });
    else queueMicrotask(done);
    return this;
  };
  Writable.prototype.destroy = function (err) {
    if (this._destroyed) return this;
    this._destroyed = true;
    this.writable = false;
    if (this._destroyImpl) this._destroyImpl(err || null, () => {});
    queueMicrotask(() => { if (err) this.emit("error", err); this.emit("close"); });
    return this;
  };
  Writable.toWeb = function (nodeWritable) {
    return new WritableStream({
      write(chunk) { return new Promise((res, rej) => nodeWritable.write(chunk, (e) => e ? rej(e) : res())); },
      close() { return new Promise((res) => nodeWritable.end(res)); },
      abort(reason) { nodeWritable.destroy(reason); },
    });
  };

  // Duplex is both halves on one object; Transform is a Duplex whose write
  // feeds its own readable side through _transform.
  function Duplex(options) {
    Readable.call(this, options);
    Writable.call(this, options);
    if (options && typeof options.write === "function") this._write = options.write;
    if (options && typeof options.read === "function") this._read = options.read;
  }
  Duplex.prototype = Object.create(Readable.prototype);
  Duplex.prototype.constructor = Duplex;
  for (const k of ["write", "end", "cork", "uncork", "_write"])
    Duplex.prototype[k] = Writable.prototype[k];
  Object.defineProperty(Duplex.prototype, "writable", {
    get() { return this._writable !== false; },
    set(v) { this._writable = v; },
    configurable: true,
  });

  function Transform(options) {
    Duplex.call(this, options);
    this._writableObjectMode = !!(options && options.objectMode);
    this._readableObjectMode = !!(options && options.objectMode);
    if (options && typeof options.transform === "function") this._transform = options.transform;
    if (options && typeof options.flush === "function") this._flush = options.flush;
  }
  Transform.prototype = Object.create(Duplex.prototype);
  Transform.prototype.constructor = Transform;
  Transform.prototype._transform = function (chunk, enc, cb) { cb(null, chunk); };
  Transform.prototype._write = function (chunk, enc, cb) {
    this._transform(chunk, enc, (err, out) => {
      if (err) return cb(err);
      if (out !== undefined && out !== null) this.push(out);
      cb();
    });
  };
  Transform.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (chunk !== undefined && chunk !== null) this.write(chunk, enc);
    const finish = () => { this.push(null); if (cb) cb(); this.emit("finish"); };
    if (this._flush) this._flush((err, out) => {
      if (err) return this.emit("error", err);
      if (out !== undefined && out !== null) this.push(out);
      finish();
    });
    else queueMicrotask(finish);
    return this;
  };

  function PassThrough(options) { Transform.call(this, options); }
  PassThrough.prototype = Object.create(Transform.prototype);
  PassThrough.prototype.constructor = PassThrough;

  function finished(stream, cb) {
    const done = (err) => { cleanup(); cb ? cb(err) : undefined; if (!cb) throw err; };
    const onEnd = () => { cleanup(); if (cb) cb(null); };
    const onErr = (e) => { cleanup(); if (cb) cb(e); };
    const cleanup = () => {
      stream.off && stream.off("end", onEnd);
      stream.off && stream.off("finish", onEnd);
      stream.off && stream.off("error", onErr);
    };
    void done;
    stream.on("end", onEnd); stream.on("finish", onEnd); stream.on("error", onErr);
  }

  function pipeline(...args) {
    let cb = typeof args[args.length - 1] === "function" ? args.pop() : null;
    const streams = args;
    let cur = streams[0];
    for (let i = 1; i < streams.length; i++) cur = cur.pipe(streams[i]);
    const last = streams[streams.length - 1];
    if (cb) finished(last, cb);
    return last;
  }

  const streamModule = {
    Readable, Writable, Duplex, Transform, PassThrough,
    pipeline, finished,
    promises: {
      pipeline: (...a) => new Promise((res, rej) =>
        pipeline(...a, (e) => (e ? rej(e) : res(undefined)))),
      finished: (s) => new Promise((res, rej) => finished(s, (e) => (e ? rej(e) : res(undefined)))),
    },
  };
  streamModule.Stream = Readable;
  globalThis.__sxnStream = streamModule;
  globalThis.__sxnStreamPromises = streamModule.promises;


  // ---------------- node:http ----------------
  // Node's (req, res) server on top of Sxn.serve. The handler here returns a
  // promise that settles when res.end() runs, which is what lets an
  // application write its response whenever it likes rather than returning
  // one -- the shape Express and everything like it is built around.
  const STATUS_CODES = {
    200: "OK", 201: "Created", 202: "Accepted", 204: "No Content",
    301: "Moved Permanently", 302: "Found", 303: "See Other", 304: "Not Modified",
    307: "Temporary Redirect", 308: "Permanent Redirect",
    400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
    405: "Method Not Allowed", 409: "Conflict", 410: "Gone",
    413: "Payload Too Large", 415: "Unsupported Media Type", 418: "I'm a Teapot",
    422: "Unprocessable Entity", 429: "Too Many Requests",
    500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway",
    503: "Service Unavailable", 504: "Gateway Timeout",
  };

  function IncomingMessage(raw) {
    Readable.call(this, {});
    this.method = raw.method || "GET";
    this.url = raw.url || "/";
    this.headers = {};
    for (const k of Object.keys(raw.headers || {}))
      this.headers[k.toLowerCase()] = raw.headers[k];
    this.httpVersion = "1.1";
    this.rawHeaders = [];
    for (const k of Object.keys(this.headers)) this.rawHeaders.push(k, this.headers[k]);
    this.socket = { remoteAddress: "127.0.0.1", encrypted: false };
    this.connection = this.socket;
    this.complete = true;
    // The body has already been read off the wire, so it arrives as one chunk.
    const body = raw.body;
    queueMicrotask(() => {
      if (body) this.push(body);
      this.push(null);
    });
  }
  IncomingMessage.prototype = Object.create(Readable.prototype);
  IncomingMessage.prototype.constructor = IncomingMessage;

  function ServerResponse(settle) {
    Writable.call(this, {});
    this.statusCode = 200;
    this.statusMessage = undefined;
    this.headersSent = false;
    this.finished = false;
    this._headers = {};
    this._chunks = [];
    this._settle = settle;
  }
  ServerResponse.prototype = Object.create(Writable.prototype);
  ServerResponse.prototype.constructor = ServerResponse;
  ServerResponse.prototype.setHeader = function (name, value) {
    this._headers[String(name).toLowerCase()] = value;
    return this;
  };
  ServerResponse.prototype.getHeader = function (name) { return this._headers[String(name).toLowerCase()]; };
  ServerResponse.prototype.getHeaders = function () { return Object.assign({}, this._headers); };
  ServerResponse.prototype.getHeaderNames = function () { return Object.keys(this._headers); };
  ServerResponse.prototype.hasHeader = function (name) {
    return Object.prototype.hasOwnProperty.call(this._headers, String(name).toLowerCase());
  };
  ServerResponse.prototype.removeHeader = function (name) { delete this._headers[String(name).toLowerCase()]; };
  ServerResponse.prototype.writeHead = function (status, reasonOrHeaders, maybeHeaders) {
    this.statusCode = status;
    let headers = maybeHeaders;
    if (typeof reasonOrHeaders === "string") this.statusMessage = reasonOrHeaders;
    else headers = reasonOrHeaders || maybeHeaders;
    if (headers) for (const k of Object.keys(headers)) this.setHeader(k, headers[k]);
    this.headersSent = true;
    return this;
  };
  ServerResponse.prototype.write = function (chunk, enc, cb) {
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    this.headersSent = true;
    if (cb) queueMicrotask(cb);
    return true;
  };
  ServerResponse.prototype.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; }
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (this.finished) return this;
    if (chunk !== undefined && chunk !== null) this._chunks.push(chunk);
    this.finished = true;
    this.headersSent = true;
    // Concatenate once: text chunks join, binary chunks merge into one array.
    let body;
    const anyBinary = this._chunks.some((c) => c instanceof Uint8Array || c instanceof ArrayBuffer);
    if (anyBinary) {
      const parts = this._chunks.map((c) =>
        c instanceof Uint8Array ? c
        : c instanceof ArrayBuffer ? new Uint8Array(c)
        : new TextEncoder().encode(String(c)));
      let total = 0; for (const p of parts) total += p.length;
      body = new Uint8Array(total);
      let at = 0; for (const p of parts) { body.set(p, at); at += p.length; }
    } else {
      body = this._chunks.map((c) => String(c)).join("");
    }
    this._settle({ statusCode: this.statusCode, headers: this._headers, body });
    if (cb) queueMicrotask(cb);
    this.emit("finish");
    this.emit("close");
    return this;
  };

  function Server(handler) {
    EE.call(this);
    this._handler = handler || null;
    this._native = null;
    if (handler) this.on("request", handler);
  }
  inheritEE(Server);
  Server.prototype.listen = function (port, hostOrCb, cb) {
    if (typeof hostOrCb === "function") { cb = hostOrCb; }
    if (typeof port === "object" && port !== null) port = port.port;
    const self = this;
    this._native = Sxn.serve({ port: Number(port) || 0 }, (raw) =>
      new Promise((resolve) => {
        const req = new IncomingMessage(raw);
        const res = new ServerResponse(resolve);
        self.emit("request", req, res);
      }));
    this._port = this._native.port;
    if (cb) queueMicrotask(() => cb());
    queueMicrotask(() => self.emit("listening"));
    return this;
  };
  Server.prototype.address = function () {
    return this._native ? { address: "127.0.0.1", family: "IPv4", port: this._native.port } : null;
  };
  Server.prototype.close = function (cb) {
    if (this._native) { this._native.stop(); this._native = null; }
    if (cb) queueMicrotask(() => cb());
    queueMicrotask(() => this.emit("close"));
    return this;
  };

  // The client half maps onto fetch, which is the transport this runtime has.
  function ClientRequest(options, cb) {
    Writable.call(this, {});
    const opts = typeof options === "string" ? { url: options } : options || {};
    this._url = opts.url ||
      ((opts.protocol || "http:") + "//" + (opts.hostname || opts.host || "localhost") +
       (opts.port ? ":" + opts.port : "") + (opts.path || "/"));
    this._method = opts.method || "GET";
    this._headers = Object.assign({}, opts.headers);
    this._body = [];
    if (cb) this.once("response", cb);
  }
  ClientRequest.prototype = Object.create(Writable.prototype);
  ClientRequest.prototype.constructor = ClientRequest;
  ClientRequest.prototype.setHeader = function (k, v) { this._headers[k] = v; return this; };
  ClientRequest.prototype.write = function (chunk) { this._body.push(chunk); return true; };
  ClientRequest.prototype.end = function (chunk) {
    if (chunk !== undefined && chunk !== null) this._body.push(chunk);
    const body = this._body.length ? this._body.map(String).join("") : undefined;
    fetch(this._url, { method: this._method, headers: this._headers, body }).then(async (r) => {
      const res = new IncomingMessage({
        method: this._method, url: this._url,
        headers: Object.fromEntries(r.headers), body: await r.text(),
      });
      res.statusCode = r.status;
      res.statusMessage = r.statusText;
      this.emit("response", res);
    }, (e) => this.emit("error", e));
    return this;
  };

  const http = {
    STATUS_CODES,
    METHODS: ["DELETE","GET","HEAD","OPTIONS","PATCH","POST","PUT"],
    IncomingMessage, ServerResponse, Server,
    createServer(opts, handler) {
      if (typeof opts === "function") { handler = opts; }
      return new Server(handler);
    },
    request(options, cb) { return new ClientRequest(options, cb); },
    get(options, cb) { const r = new ClientRequest(options, cb); r.end(); return r; },
  };
  globalThis.__sxnHttp = http;


  // ---------------- small node builtins ----------------
  // Enough of each for the packages that reach for them in passing: `debug`
  // wants tty.isatty, body-parser wants string_decoder, and so on.
  const tty = {
    isatty: () => false,
    ReadStream: function ReadStream() { throw new Error("tty.ReadStream is not supported"); },
    WriteStream: function WriteStream() { throw new Error("tty.WriteStream is not supported"); },
  };
  globalThis.__sxnTty = tty;

  // Decodes byte chunks to text without splitting a multi-byte character
  // across a chunk boundary -- which is the entire reason it exists.
  function StringDecoder(encoding) {
    this.encoding = (encoding || "utf8").toLowerCase();
    this._dec = new TextDecoder(this.encoding === "utf8" ? "utf-8" : this.encoding);
  }
  StringDecoder.prototype.write = function (buf) {
    if (typeof buf === "string") return buf;
    return this._dec.decode(buf, { stream: true });
  };
  StringDecoder.prototype.end = function (buf) {
    let out = buf ? this.write(buf) : "";
    out += this._dec.decode();
    return out;
  };
  globalThis.__sxnStringDecoder = { StringDecoder };

  const timers = {
    setTimeout: (...a) => globalThis.setTimeout(...a),
    clearTimeout: (...a) => globalThis.clearTimeout(...a),
    setInterval: (...a) => globalThis.setInterval(...a),
    clearInterval: (...a) => globalThis.clearInterval(...a),
    setImmediate: (fn, ...a) => globalThis.setTimeout(() => fn(...a), 0),
    clearImmediate: (id) => globalThis.clearTimeout(id),
    promises: {
      setTimeout: (ms, value) => new Promise((r) => globalThis.setTimeout(() => r(value), ms)),
      setImmediate: (value) => new Promise((r) => globalThis.setTimeout(() => r(value), 0)),
    },
  };
  globalThis.__sxnTimers = timers;
  globalThis.__sxnTimersPromises = timers.promises;
  if (typeof globalThis.setImmediate === "undefined") globalThis.setImmediate = timers.setImmediate;
  if (typeof globalThis.clearImmediate === "undefined") globalThis.clearImmediate = timers.clearImmediate;

  const perfHooks = {
    performance: globalThis.performance,
    PerformanceObserver: function PerformanceObserver() {
      throw new Error("PerformanceObserver is not supported");
    },
  };
  globalThis.__sxnPerfHooks = perfHooks;

  // node:module -- createRequire is what ESM code uses to reach CommonJS.
  const moduleModule = {
    createRequire: () => globalThis.require,
    builtinModules: ["assert","buffer","events","fs","http","os","path","process",
                     "querystring","stream","string_decoder","timers","tty","url","util"],
    isBuiltin: (n) => moduleModule.builtinModules.includes(String(n).replace(/^node:/, "")),
  };
  globalThis.__sxnModule = moduleModule;

  // CommonJS reaches the builtins through require(), which is synchronous, so
  // it cannot go through import(). Every builtin is already a plain object on
  // a global; this maps a specifier to it, with or without the node: prefix.
  globalThis.__sxnBuiltinRequire = function (specifier) {
    const name = String(specifier).replace(/^node:/, "");
    const table = {
      buffer: { Buffer, default: Buffer },
      events: globalThis.__sxnEventEmitter,
      path: globalThis.__sxnPath,
      process: globalThis.process,
      fs: globalThis.__sxnFs,
      "fs/promises": globalThis.__sxnFsPromises,
      util: globalThis.__sxnUtil,
      os: globalThis.__sxnOs,
      url: globalThis.__sxnUrl,
      querystring: globalThis.__sxnQuerystring,
      assert: globalThis.__sxnAssert,
      "assert/strict": globalThis.__sxnAssert,
      stream: globalThis.__sxnStream,
      "stream/promises": globalThis.__sxnStream && globalThis.__sxnStream.promises,
      http: globalThis.__sxnHttp,
      tty: globalThis.__sxnTty,
      string_decoder: globalThis.__sxnStringDecoder,
      timers: globalThis.__sxnTimers,
      "timers/promises": globalThis.__sxnTimers && globalThis.__sxnTimers.promises,
      perf_hooks: globalThis.__sxnPerfHooks,
      module: globalThis.__sxnModule,
    };
    const m = table[name];
    if (m === undefined) {
      const e = new Error("Cannot find module '" + specifier + "'");
      e.code = "MODULE_NOT_FOUND";
      throw e;
    }
    // events exports the constructor itself, and Node lets you reach the
    // named helpers off it either way.
    return m;
  };
  globalThis.__sxnIsBuiltin = function (specifier) {
    try { globalThis.__sxnBuiltinRequire(specifier); return true; } catch { return false; }
  };

  // ---------------- node:util ----------------
  // The parts packages actually import: promisify, callbackify, inherits,
  // format, deprecate, and the types guards. inspect is a readable
  // approximation, not Node's exact formatter.
  function inspect(v, opts, depth) {
    opts = opts || {};
    const max = opts.depth === undefined ? 2 : opts.depth;
    const seen = opts._seen || new Set();
    depth = depth || 0;
    const t = typeof v;
    if (v === null) return "null";
    if (t === "string") return depth === 0 && !opts.quoteStrings ? v : JSON.stringify(v);
    if (t === "number" || t === "boolean" || t === "undefined") return String(v);
    if (t === "bigint") return String(v) + "n";
    if (t === "symbol") return v.toString();
    if (t === "function") return "[Function: " + (v.name || "anonymous") + "]";
    if (v instanceof Error) return v.stack || (v.name + ": " + v.message);
    if (v instanceof Date) return v.toISOString();
    if (v instanceof RegExp) return String(v);
    if (seen.has(v)) return "[Circular *1]";
    if (depth > max) return Array.isArray(v) ? "[Array]" : "[Object]";
    seen.add(v);
    const sub = Object.assign({}, opts, { _seen: seen, quoteStrings: true });
    let out;
    if (Array.isArray(v)) {
      out = "[ " + v.map((e) => inspect(e, sub, depth + 1)).join(", ") + " ]";
      if (v.length === 0) out = "[]";
    } else if (v instanceof Map) {
      out = "Map(" + v.size + ") {" + (v.size ? " " + [...v].map(([k, val]) =>
        inspect(k, sub, depth + 1) + " => " + inspect(val, sub, depth + 1)).join(", ") + " " : "") + "}";
    } else if (v instanceof Set) {
      out = "Set(" + v.size + ") {" + (v.size ? " " + [...v].map((e) =>
        inspect(e, sub, depth + 1)).join(", ") + " " : "") + "}";
    } else if (ArrayBuffer.isView(v)) {
      out = v.constructor.name + "(" + v.length + ") [ " + Array.from(v).join(", ") + " ]";
    } else {
      const keys = Object.keys(v);
      out = keys.length === 0 ? "{}" : "{ " + keys.map((k) =>
        (/^[A-Za-z_$][\w$]*$/.test(k) ? k : JSON.stringify(k)) + ": " +
        inspect(v[k], sub, depth + 1)).join(", ") + " }";
    }
    seen.delete(v);
    return out;
  }

  function format(...args) {
    if (typeof args[0] !== "string") return args.map((a) => inspect(a)).join(" ");
    // With nothing to substitute, Node returns the string untouched -- even
    // "%%" stays as written.
    if (args.length === 1) return args[0];
    let i = 1;
    let out = args[0].replace(/%[sdifjoOc%]/g, (m) => {
      if (m === "%%") return "%";
      if (i >= args.length) return m;
      const a = args[i++];
      switch (m) {
        case "%s": return typeof a === "string" ? a : inspect(a);
        case "%d": case "%f": return typeof a === "bigint" ? a + "n" : Number(a).toString();
        case "%i": return typeof a === "bigint" ? a + "n" : parseInt(a, 10).toString();
        case "%j": try { return JSON.stringify(a); } catch { return "[Circular]"; }
        case "%o": case "%O": return inspect(a, { depth: 4 });
        case "%c": return "";
        default: return m;
      }
    });
    for (; i < args.length; i++) out += " " + (typeof args[i] === "string" ? args[i] : inspect(args[i]));
    return out;
  }

  const util = {
    inspect,
    format,
    // Node keeps the custom-inspect symbol here.
    promisify(fn) {
      if (typeof fn !== "function") throw new TypeError("promisify expects a function");
      const wrapped = function (...args) {
        return new Promise((resolve, reject) => {
          fn.call(this, ...args, (err, ...values) =>
            err ? reject(err) : resolve(values.length > 1 ? values : values[0]));
        });
      };
      Object.defineProperty(wrapped, "name", { value: fn.name, configurable: true });
      return wrapped;
    },
    callbackify(fn) {
      return function (...args) {
        const cb = args.pop();
        Promise.resolve(fn.apply(this, args)).then((v) => cb(null, v), (e) => cb(e || new Error("rejected")));
      };
    },
    inherits(ctor, superCtor) {
      Object.defineProperty(ctor, "super_", { value: superCtor, writable: true, configurable: true });
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    },
    deprecate(fn, msg) {
      let warned = false;
      return function (...args) {
        if (!warned) { warned = true; console.error("DeprecationWarning: " + msg); }
        return fn.apply(this, args);
      };
    },
    isDeepStrictEqual(a, b) { return deepEqual(a, b, true); },
    types: {
      isDate: (v) => v instanceof Date,
      isRegExp: (v) => v instanceof RegExp,
      isMap: (v) => v instanceof Map,
      isSet: (v) => v instanceof Set,
      isPromise: (v) => v instanceof Promise,
      isTypedArray: (v) => ArrayBuffer.isView(v) && !(v instanceof DataView),
      isArrayBuffer: (v) => v instanceof ArrayBuffer,
      isDataView: (v) => v instanceof DataView,
      isNativeError: (v) => v instanceof Error,
      isAsyncFunction: (v) => typeof v === "function" && v.constructor && v.constructor.name === "AsyncFunction",
      isGeneratorFunction: (v) => typeof v === "function" && v.constructor && v.constructor.name === "GeneratorFunction",
    },
    TextEncoder: globalThis.TextEncoder,
    TextDecoder: globalThis.TextDecoder,
  };
  globalThis.__sxnUtil = util;

  // Structural equality, shared by util.isDeepStrictEqual and node:assert.
  function deepEqual(a, b, strict) {
    if (strict ? Object.is(a, b) : a == b) return true;
    if (a === null || b === null || typeof a !== "object" || typeof b !== "object") {
      return strict ? Object.is(a, b) : a == b;
    }
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    if (a instanceof Date) return a.getTime() === b.getTime();
    if (a instanceof RegExp) return String(a) === String(b);
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    if (ArrayBuffer.isView(a)) {
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
      return true;
    }
    if (a instanceof Map) {
      if (a.size !== b.size) return false;
      for (const [k, v] of a) { if (!b.has(k) || !deepEqual(v, b.get(k), strict)) return false; }
      return true;
    }
    if (a instanceof Set) {
      if (a.size !== b.size) return false;
      for (const v of a) if (!b.has(v)) return false;
      return true;
    }
    const ka = Object.keys(a), kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
      if (!deepEqual(a[k], b[k], strict)) return false;
    }
    return true;
  }

  // ---------------- node:assert ----------------
  function AssertionError(opts) {
    const e = new Error(opts.message ||
      (inspect(opts.actual) + " " + opts.operator + " " + inspect(opts.expected)));
    e.name = "AssertionError";
    e.code = "ERR_ASSERTION";
    e.actual = opts.actual; e.expected = opts.expected; e.operator = opts.operator;
    return e;
  }
  function assert(value, message) {
    if (!value) throw AssertionError({ message, actual: value, expected: true, operator: "==" });
  }
  assert.ok = assert;
  assert.AssertionError = AssertionError;
  assert.equal = (a, b, m) => { if (!(a == b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "==" }); };
  assert.notEqual = (a, b, m) => { if (a == b) throw AssertionError({ message: m, actual: a, expected: b, operator: "!=" }); };
  assert.strictEqual = (a, b, m) => { if (!Object.is(a, b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "strictEqual" }); };
  assert.notStrictEqual = (a, b, m) => { if (Object.is(a, b)) throw AssertionError({ message: m, actual: a, expected: b, operator: "notStrictEqual" }); };
  assert.deepEqual = (a, b, m) => { if (!deepEqual(a, b, false)) throw AssertionError({ message: m, actual: a, expected: b, operator: "deepEqual" }); };
  assert.deepStrictEqual = (a, b, m) => { if (!deepEqual(a, b, true)) throw AssertionError({ message: m, actual: a, expected: b, operator: "deepStrictEqual" }); };
  assert.notDeepStrictEqual = (a, b, m) => { if (deepEqual(a, b, true)) throw AssertionError({ message: m, actual: a, expected: b, operator: "notDeepStrictEqual" }); };
  assert.fail = (m) => { throw AssertionError({ message: m || "Failed", operator: "fail" }); };
  assert.throws = (fn, _expected, m) => {
    try { fn(); } catch { return; }
    throw AssertionError({ message: m || "Missing expected exception.", operator: "throws" });
  };
  assert.doesNotThrow = (fn, m) => { fn(); void m; };
  assert.match = (str, re, m) => { if (!re.test(str)) throw AssertionError({ message: m, actual: str, expected: re, operator: "match" }); };
  assert.strict = assert;
  globalThis.__sxnAssert = assert;

  // ---------------- node:os ----------------
  const os = {
    EOL: "\n",
    platform: () => process.platform,
    arch: () => process.arch,
    type: () => (process.platform === "darwin" ? "Darwin" : process.platform === "win32" ? "Windows_NT" : "Linux"),
    release: () => "",
    hostname: () => "localhost",
    tmpdir: () => (process.env && (process.env.TMPDIR || process.env.TMP)) || "/tmp",
    homedir: () => (process.env && process.env.HOME) || "/",
    endianness: () => "LE",
    cpus: () => [],
    totalmem: () => 0,
    freemem: () => 0,
    uptime: () => Math.floor(performance.now() / 1000),
    devNull: "/dev/null",
  };
  globalThis.__sxnOs = os;

  // ---------------- node:querystring ----------------
  const querystring = {
    parse(str, sep, eq) {
      const out = Object.create(null);
      if (typeof str !== "string" || str.length === 0) return out;
      for (const part of str.split(sep || "&")) {
        if (!part) continue;
        const i = part.indexOf(eq || "=");
        const k = decodeURIComponent((i < 0 ? part : part.slice(0, i)).replace(/\+/g, " "));
        const v = i < 0 ? "" : decodeURIComponent(part.slice(i + 1).replace(/\+/g, " "));
        if (k in out) { if (Array.isArray(out[k])) out[k].push(v); else out[k] = [out[k], v]; }
        else out[k] = v;
      }
      return out;
    },
    stringify(obj, sep, eq) {
      if (!obj || typeof obj !== "object") return "";
      const parts = [];
      for (const k of Object.keys(obj)) {
        const v = obj[k];
        const ek = encodeURIComponent(k);
        if (Array.isArray(v)) for (const one of v) parts.push(ek + (eq || "=") + encodeURIComponent(one));
        else parts.push(ek + (eq || "=") + encodeURIComponent(v === undefined || v === null ? "" : v));
      }
      return parts.join(sep || "&");
    },
    escape: encodeURIComponent,
    unescape: decodeURIComponent,
  };
  globalThis.__sxnQuerystring = querystring;

  // ---------------- node:url ----------------
  const url = {
    URL: globalThis.URL,
    URLSearchParams: globalThis.URLSearchParams,
    fileURLToPath(u) {
      const s = typeof u === "string" ? u : String(u);
      if (!s.startsWith("file://")) throw new TypeError("must be a file: URL");
      return decodeURIComponent(s.slice(7).replace(/^localhost/, "")) || "/";
    },
    pathToFileURL(p) {
      return new URL("file://" + encodeURI(String(p)).replace(/[?#]/g, encodeURIComponent));
    },
    format: (u) => String(u),
    parse: (s) => { try { return new URL(s); } catch { return null; } },
  };
  globalThis.__sxnUrl = url;
})();
