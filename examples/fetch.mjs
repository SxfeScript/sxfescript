// fetch and Web Streams. Run it: sxn examples/fetch.mjs
//
// fetch is the global one from the Fetch standard, backed by libcurl. The
// response body is a real ReadableStream, so it can be consumed a chunk at a
// time instead of all at once.

const res = await fetch("https://example.com/");
console.log(res.status, res.headers.get("content-type"));

// Read it as text, in whole.
const html = await res.text();
console.log(`${html.length} bytes`);

// Or a chunk at a time, decoding as the bytes arrive.
const streamed = await fetch("https://example.com/");
let chunks = 0;
let characters = 0;
for await (const chunk of streamed.body.pipeThrough(new TextDecoderStream())) {
  chunks += 1;
  characters += chunk.length;
}
console.log(`${chunks} chunk(s), ${characters} characters`);
