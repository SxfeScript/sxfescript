// Bun's own server API, matching server.sx's Sxn.serve and the same response.
Bun.serve({ port: 8996, fetch: () => new Response('{"items":[1,2,3,4,5],"name":"payload"}') });
