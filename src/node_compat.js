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
  // plain `this._events` object/arrays the native side reads and writes
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
})();
