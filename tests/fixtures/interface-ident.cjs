/* Regression test: `interface` is a contextual SX/TypeScript declaration
   keyword, not a real JS keyword. It must remain usable as an ordinary
   identifier in plain .js/.mjs/.cjs source that the native parser also
   handles (see update_token_ident/js_parse_statement_or_decl in
   third_party/quickjs/quickjs.c). */
{
    let interface = 1;
    if (interface !== 1) throw new Error("interface as let binding failed");
}

{
    function interface() { return 42; }
    if (interface() !== 42) throw new Error("interface as function name failed");
}

const obj = { interface: 5 };
if (obj.interface !== 5) throw new Error("interface as property name failed");
