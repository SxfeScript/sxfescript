const vscode = require('vscode');
const cp = require('child_process');

/* Just enough LSP to carry semantic tokens. The alternative is
   vscode-languageclient, which is a dependency and a bundling step for one
   request and one notification; this is the request and the notification. */
class Server {
  constructor(command, output) {
    this.output = output;
    this.pending = new Map();
    this.nextId = 1;
    this.buffer = Buffer.alloc(0);
    this.child = cp.spawn(command, ['lsp', '--stdio'], { stdio: ['pipe', 'pipe', 'pipe'] });
    this.child.stdout.on('data', chunk => this.read(chunk));
    this.child.stderr.on('data', data => output.append(data.toString()));
    this.child.on('error', error => output.appendLine(`Unable to start SXN LSP: ${error.message}`));
    this.request('initialize', { processId: process.pid, rootUri: null, capabilities: {} })
      .then(() => this.notify('initialized', {}))
      .catch(error => output.appendLine(`SXN LSP did not initialize: ${error.message}`));
  }

  send(message) {
    if (this.child.exitCode !== null) return;
    const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', ...message }), 'utf8');
    this.child.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.child.stdin.write(body);
  }

  notify(method, params) { this.send({ method, params }); }

  request(method, params) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.send({ id, method, params });
    });
  }

  /* Responses arrive as Content-Length framed JSON, split across chunks
     wherever the pipe happened to break. */
  read(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const split = this.buffer.indexOf('\r\n\r\n');
      if (split < 0) return;
      const header = this.buffer.slice(0, split).toString('ascii');
      const length = Number((header.match(/Content-Length: (\d+)/i) || [])[1]);
      if (!Number.isInteger(length)) { this.buffer = this.buffer.slice(split + 4); continue; }
      if (this.buffer.length < split + 4 + length) return;
      const body = this.buffer.slice(split + 4, split + 4 + length).toString('utf8');
      this.buffer = this.buffer.slice(split + 4 + length);
      let message;
      try { message = JSON.parse(body); } catch (error) { this.output.appendLine(`SXN LSP sent unreadable JSON: ${error.message}`); continue; }
      const waiting = this.pending.get(message.id);
      if (!waiting) continue;
      this.pending.delete(message.id);
      if (message.error) waiting.reject(new Error(message.error.message));
      else waiting.resolve(message.result);
    }
  }

  dispose() {
    for (const waiting of this.pending.values()) waiting.reject(new Error('SXN LSP stopped'));
    this.pending.clear();
    this.child.kill();
  }
}

/* Must match the legend src/lsp.c sends back in its initialize result. */
const LEGEND = new vscode.SemanticTokensLegend(['keyword', 'operator'], []);

function activate(context) {
  const command = vscode.workspace.getConfiguration('sxfe').get('sxnPath', 'sxn');
  const output = vscode.window.createOutputChannel('SxfeScript');
  const server = new Server(command, output);
  const open = new Set();

  /* The server syncs whole documents, so a change sends the whole text. */
  const sync = document => {
    if (document.languageId !== 'sxfe') return;
    const uri = document.uri.toString();
    const item = { uri, languageId: 'sxfe', version: document.version, text: document.getText() };
    if (open.has(uri)) server.notify('textDocument/didChange', { textDocument: item, contentChanges: [{ text: item.text }] });
    else { open.add(uri); server.notify('textDocument/didOpen', { textDocument: item }); }
  };

  context.subscriptions.push(
    output,
    server,
    vscode.workspace.onDidOpenTextDocument(sync),
    vscode.workspace.onDidChangeTextDocument(event => sync(event.document)),
    vscode.workspace.onDidCloseTextDocument(document => {
      const uri = document.uri.toString();
      if (!open.delete(uri)) return;
      server.notify('textDocument/didClose', { textDocument: { uri } });
    }),
    vscode.languages.registerDocumentSemanticTokensProvider({ language: 'sxfe' }, {
      async provideDocumentSemanticTokens(document) {
        sync(document);
        const result = await server.request('textDocument/semanticTokens/full', {
          textDocument: { uri: document.uri.toString() },
        });
        /* The server already delta-encodes, which is the same shape
           SemanticTokens takes, so the data passes straight through. */
        return result && result.data ? new vscode.SemanticTokens(Uint32Array.from(result.data)) : null;
      },
    }, LEGEND),
  );

  vscode.workspace.textDocuments.forEach(sync);
}

function deactivate() {}
module.exports = { activate, deactivate };
