// Axon for VS Code: starts `axon lsp` and connects it to the editor.
// This is a small Language Server Protocol client written against the plain VS Code API,
// so the extension has no npm dependencies.
const vscode = require("vscode");
const cp = require("child_process");

let server = null;
let nextId = 1;
const pending = new Map();
let buffer = Buffer.alloc(0);
let diagnostics = null;
let output = null;

function send(msg) {
  if (!server) return;
  const body = Buffer.from(JSON.stringify(msg), "utf8");
  server.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  server.stdin.write(body);
}

function request(method, params) {
  const id = nextId++;
  send({ jsonrpc: "2.0", id, method, params });
  return new Promise((resolve) => pending.set(id, resolve));
}

function notify(method, params) {
  send({ jsonrpc: "2.0", method, params });
}

function onData(chunk) {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    const headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    const header = buffer.slice(0, headerEnd).toString("ascii");
    const m = /Content-Length: (\d+)/i.exec(header);
    if (!m) { buffer = buffer.slice(headerEnd + 4); continue; }
    const len = parseInt(m[1], 10);
    if (buffer.length < headerEnd + 4 + len) return;
    const body = buffer.slice(headerEnd + 4, headerEnd + 4 + len).toString("utf8");
    buffer = buffer.slice(headerEnd + 4 + len);
    let msg;
    try { msg = JSON.parse(body); } catch (e) { continue; }
    if (msg.id !== undefined && pending.has(msg.id)) {
      pending.get(msg.id)(msg.result);
      pending.delete(msg.id);
    } else if (msg.method === "textDocument/publishDiagnostics") {
      showDiagnostics(msg.params);
    }
  }
}

const toPos = (p) => new vscode.Position(p.line, p.character);
const toRange = (r) => new vscode.Range(toPos(r.start), toPos(r.end));
const docParams = (doc) => ({ textDocument: { uri: doc.uri.toString() } });
const posParams = (doc, pos) => ({ textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } });

function showDiagnostics(params) {
  const uri = vscode.Uri.parse(params.uri);
  diagnostics.set(uri, params.diagnostics.map((d) => {
    const diag = new vscode.Diagnostic(toRange(d.range), d.message,
      d.severity === 1 ? vscode.DiagnosticSeverity.Error : vscode.DiagnosticSeverity.Warning);
    diag.source = "axon";
    return diag;
  }));
}

const completionKinds = {
  2: vscode.CompletionItemKind.Method, 3: vscode.CompletionItemKind.Function, 5: vscode.CompletionItemKind.Field,
  6: vscode.CompletionItemKind.Variable, 8: vscode.CompletionItemKind.Interface, 13: vscode.CompletionItemKind.Enum,
  14: vscode.CompletionItemKind.Keyword, 20: vscode.CompletionItemKind.EnumMember, 21: vscode.CompletionItemKind.Constant,
  22: vscode.CompletionItemKind.Struct,
};

function symbolOf(s, uri) {
  const sym = new vscode.DocumentSymbol(s.name, "", s.kind - 1, toRange(s.range), toRange(s.selectionRange));
  sym.children = (s.children || []).map((c) => symbolOf(c, uri));
  return sym;
}

function start(context) {
  const exe = vscode.workspace.getConfiguration("axon").get("path") || "axon";
  output.appendLine(`starting ${exe} lsp`);
  server = cp.spawn(exe, ["lsp"], { stdio: ["pipe", "pipe", "pipe"] });
  server.on("error", (e) => {
    vscode.window.showErrorMessage(`Axon: couldn't start "${exe} lsp" (${e.message}). Set "axon.path" in your settings.`);
    server = null;
  });
  server.stdout.on("data", onData);
  server.stderr.on("data", (d) => output.append(d.toString()));
  server.on("exit", (code) => output.appendLine(`axon lsp exited (${code})`));
  return request("initialize", { processId: process.pid, rootUri: null, capabilities: {} }).then(() => {
    notify("initialized", {});
    for (const doc of vscode.workspace.textDocuments) open(doc);
  });
}

function open(doc) {
  if (doc.languageId !== "axon") return;
  notify("textDocument/didOpen", { textDocument: { uri: doc.uri.toString(), languageId: "axon", version: doc.version, text: doc.getText() } });
}

function activate(context) {
  output = vscode.window.createOutputChannel("Axon");
  diagnostics = vscode.languages.createDiagnosticCollection("axon");
  context.subscriptions.push(output, diagnostics);
  start(context);

  const sel = { language: "axon" };
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(open),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId !== "axon") return;
      notify("textDocument/didChange", {
        textDocument: { uri: e.document.uri.toString(), version: e.document.version },
        contentChanges: [{ text: e.document.getText() }],
      });
    }),
    vscode.workspace.onDidSaveTextDocument((doc) => {
      if (doc.languageId === "axon") notify("textDocument/didSave", docParams(doc));
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId !== "axon") return;
      notify("textDocument/didClose", docParams(doc));
      diagnostics.delete(doc.uri);
    }),
    vscode.languages.registerHoverProvider(sel, {
      provideHover: async (doc, pos) => {
        const r = await request("textDocument/hover", posParams(doc, pos));
        if (!r) return null;
        return new vscode.Hover(new vscode.MarkdownString(r.contents.value), r.range ? toRange(r.range) : undefined);
      },
    }),
    vscode.languages.registerDefinitionProvider(sel, {
      provideDefinition: async (doc, pos) => {
        const r = await request("textDocument/definition", posParams(doc, pos));
        if (!r) return null;
        return new vscode.Location(vscode.Uri.parse(r.uri), toRange(r.range));
      },
    }),
    vscode.languages.registerCompletionItemProvider(sel, {
      provideCompletionItems: async (doc, pos) => {
        const items = (await request("textDocument/completion", posParams(doc, pos))) || [];
        return items.map((i) => {
          const item = new vscode.CompletionItem(i.label, completionKinds[i.kind] || vscode.CompletionItemKind.Text);
          if (i.detail) item.detail = i.detail;
          return item;
        });
      },
    }, "."),
    vscode.languages.registerDocumentSymbolProvider(sel, {
      provideDocumentSymbols: async (doc) => ((await request("textDocument/documentSymbol", docParams(doc))) || []).map((s) => symbolOf(s, doc.uri)),
    }),
    vscode.languages.registerDocumentFormattingEditProvider(sel, {
      provideDocumentFormattingEdits: async (doc) => {
        const edits = (await request("textDocument/formatting", { ...docParams(doc), options: {} })) || [];
        return edits.map((e) => vscode.TextEdit.replace(toRange(e.range), e.newText));
      },
    }),
  );
}

function deactivate() {
  if (!server) return;
  return request("shutdown", null).then(() => {
    notify("exit", null);
    server = null;
  });
}

module.exports = { activate, deactivate };
