const vscode = require('vscode');
const cp = require('child_process');

function activate(context) {
  const path = vscode.workspace.getConfiguration('sxfe').get('sxnPath', 'sxn');
  const process = cp.spawn(path, ['lsp', '--stdio'], { stdio: ['pipe', 'pipe', 'pipe'] });
  const output = vscode.window.createOutputChannel('SxfeScript');
  process.stderr.on('data', data => output.append(data.toString()));
  process.on('error', error => output.appendLine(`Unable to start SXN LSP: ${error.message}`));
  context.subscriptions.push(output, { dispose: () => process.kill() });
}

function deactivate() {}
module.exports = { activate, deactivate };

