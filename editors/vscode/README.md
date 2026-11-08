# Axon for VS Code

Syntax highlighting plus everything `axon lsp` offers: errors as you type, hover for types and
signatures, go to definition, completion (including members after `.`), the outline view and
Format Document.

## Install

The extension has no dependencies, so installing it is a copy:

```bat
xcopy /E /I editors\vscode "%USERPROFILE%\.vscode\extensions\axon-lang"
```

Restart VS Code. The extension runs `axon lsp`, so `axon.exe` must be on your `PATH`, or set
`"axon.path": "D:\\Apps\\Axon\\bin\\axon.exe"` in your settings.

Other editors (Neovim, Helix, Zed, Sublime) can use the same server: point their LSP client at
`axon lsp` for `.ax` files.
