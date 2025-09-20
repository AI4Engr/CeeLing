// MADOLA Web App - Main Application Logic

class MadolaApp {
    constructor() {
        this.wasmModule = null;
        this.currentFile = null;
        this.hasUnsavedChanges = false;
        this.isElectron = window.electronAPI && window.electronAPI.isElectron;

        // Initialize MathJax configuration
        this.setupMathJax();

        // Initialize the application
        this.init();
    }

    setupMathJax() {
        window.MathJax = {
            tex: {
                inlineMath: [['$', '$'], ['\\(', '\\)']],
                displayMath: [['$$', '$$'], ['\\[', '\\]']],
                processEscapes: true,
                processEnvironments: true
            },
            options: {
                ignoreHtmlClass: 'tex2jax_ignore',
                processHtmlClass: 'tex2jax_process'
            }
        };
    }

    async init() {
        try {
            this.addMessage('info', 'Initializing MADOLA...');

            // Load WASM module (non-blocking)
            await this.loadWasm();

            // Setup event listeners
            this.setupEventListeners();

            // Setup Electron integration if available
            if (this.isElectron) {
                this.setupElectronIntegration();
            }

            // Initialize with default content
            this.loadDefaultContent();

            const status = this.wasmModule ? 'with WASM support' : 'in fallback mode';
            this.addMessage('success', `MADOLA initialized successfully ${status}!`);
        } catch (error) {
            this.addMessage('error', `Failed to initialize: ${error.message}`);
        }
    }

    async loadWasm() {
        try {
            this.addMessage('info', 'Loading MADOLA WASM module...');

            // Use the browser wrapper for MADOLA
            this.madolaWrapper = new MadolaBrowserWrapper();
            this.wasmModule = await this.madolaWrapper.loadModule();

            this.addMessage('success', 'WASM module loaded successfully');

            // Test the module
            this.addMessage('info', 'Testing WASM module...');
            const testResult = await this.madolaWrapper.evaluate('x := 42; print(x);');
            if (testResult) {
                this.addMessage('success', 'WASM module tested successfully');
            } else {
                this.addMessage('warning', 'WASM module loaded but test failed');
            }

        } catch (error) {
            this.addMessage('error', `WASM loading failed: ${error.message}`);
            this.addMessage('info', 'Running in fallback mode without WASM');
            this.wasmModule = null;
            this.madolaWrapper = null;
        }
    }

    setupEventListeners() {
        // Menu buttons
        document.getElementById('btn-new').addEventListener('click', () => this.newFile());
        document.getElementById('btn-open').addEventListener('click', () => this.openFile());
        document.getElementById('btn-save').addEventListener('click', () => this.saveFile());
        document.getElementById('btn-save-as').addEventListener('click', () => this.saveAsFile());
        document.getElementById('btn-about').addEventListener('click', () => this.showAbout());

        // Editor controls
        document.getElementById('btn-format').addEventListener('click', () => this.formatCode());
        document.getElementById('btn-run').addEventListener('click', () => this.runCode());

        // Output controls
        document.getElementById('btn-copy-output').addEventListener('click', () => this.copyOutput());
        document.getElementById('btn-download-md').addEventListener('click', () => this.downloadMarkdown());

        // Messages control
        document.getElementById('btn-clear-messages').addEventListener('click', () => this.clearMessages());

        // Modal controls
        document.getElementById('modal-close').addEventListener('click', () => this.hideAbout());
        document.getElementById('about-modal').addEventListener('click', (e) => {
            if (e.target.id === 'about-modal') this.hideAbout();
        });

        // File input (only for web version)
        if (!this.isElectron) {
            document.getElementById('file-input').addEventListener('change', (e) => this.handleFileOpen(e));
        }

        // Editor change detection
        document.getElementById('editor').addEventListener('input', () => {
            this.hasUnsavedChanges = true;
            this.updateTitle();

            // Auto-run on change (debounced)
            clearTimeout(this.autoRunTimeout);
            this.autoRunTimeout = setTimeout(() => this.runCode(), 1000);
        });

        // Keyboard shortcuts (only for web version, Electron handles these natively)
        if (!this.isElectron) {
            document.addEventListener('keydown', (e) => this.handleKeyboard(e));
        }
    }

    setupElectronIntegration() {
        // Listen for menu actions from main process
        window.electronAPI.onMenuAction((action) => {
            switch (action) {
                case 'new':
                    this.newFile();
                    break;
                case 'save':
                    this.saveFileElectron();
                    break;
                case 'run':
                    this.runCode();
                    break;
                case 'format':
                    this.formatCode();
                    break;
                case 'about':
                    this.showAbout();
                    break;
            }
        });

        // Listen for file opened from main process
        window.electronAPI.onFileOpened((fileData) => {
            document.getElementById('editor').value = fileData.content;
            this.currentFile = fileData.name;
            this.hasUnsavedChanges = false;
            this.updateTitle();
            this.addMessage('success', `Opened: ${fileData.name}`);
            this.runCode();
        });

        // Listen for save as from main process
        window.electronAPI.onSaveAs((filePath) => {
            this.saveFileToPath(filePath);
        });
    }

    loadDefaultContent() {
        const defaultCode = `//## Basic Function Example

fn test(a, b)
{
    c := 1;
    return a + b + c;
}

x := test(5, 3);
//Results:
print(x);`;

        document.getElementById('editor').value = defaultCode;
        this.runCode();
    }

    // File operations
    newFile() {
        if (this.hasUnsavedChanges) {
            if (!confirm('You have unsaved changes. Are you sure you want to create a new file?')) {
                return;
            }
        }

        document.getElementById('editor').value = '';
        document.getElementById('output').innerHTML = '<div class="output-placeholder"><p>Enter your MADOLA code to see the output here.</p></div>';
        this.currentFile = null;
        this.hasUnsavedChanges = false;
        this.updateTitle();
        this.addMessage('info', 'New file created');
    }

    openFile() {
        if (this.isElectron) {
            // Electron will handle the file dialog
            return;
        }
        document.getElementById('file-input').click();
    }

    handleFileOpen(event) {
        const file = event.target.files[0];
        if (!file) return;

        const reader = new FileReader();
        reader.onload = (e) => {
            document.getElementById('editor').value = e.target.result;
            this.currentFile = file.name;
            this.hasUnsavedChanges = false;
            this.updateTitle();
            this.addMessage('success', `Opened: ${file.name}`);
            this.runCode();
        };
        reader.readAsText(file);
    }

    saveFile() {
        if (this.isElectron) {
            this.saveFileElectron();
        } else {
            if (this.currentFile) {
                this.downloadFile(this.currentFile, document.getElementById('editor').value);
            } else {
                this.saveAsFile();
            }
        }
    }

    saveAsFile() {
        if (this.isElectron) {
            // Electron will handle the save dialog
            return;
        }

        const filename = prompt('Save as filename:', this.currentFile || 'untitled.clg');
        if (filename) {
            this.downloadFile(filename, document.getElementById('editor').value);
            this.currentFile = filename;
            this.hasUnsavedChanges = false;
            this.updateTitle();
        }
    }

    // Electron-specific save operations
    async saveFileElectron() {
        if (!this.isElectron) return;

        const currentFile = await window.electronAPI.getCurrentFile();
        if (currentFile) {
            await this.saveFileToPath(currentFile);
        }
    }

    async saveFileToPath(filePath) {
        if (!this.isElectron) return;

        try {
            const content = document.getElementById('editor').value;
            const result = await window.electronAPI.saveFile({ path: filePath, content });

            if (result.success) {
                this.currentFile = filePath.split(/[/\\]/).pop(); // Get filename from path
                this.hasUnsavedChanges = false;
                this.updateTitle();
                await window.electronAPI.setCurrentFile(filePath);
                this.addMessage('success', `Saved: ${this.currentFile}`);
            } else {
                this.addMessage('error', `Save failed: ${result.error}`);
            }
        } catch (error) {
            this.addMessage('error', `Save error: ${error.message}`);
        }
    }

    downloadFile(filename, content) {
        const blob = new Blob([content], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        a.click();
        URL.revokeObjectURL(url);
        this.addMessage('success', `Saved: ${filename}`);
    }

    // Code operations
    async formatCode() {
        if (!this.madolaWrapper || !this.madolaWrapper.isReady()) {
            this.addMessage('error', 'WASM module not loaded');
            return;
        }

        try {
            const code = document.getElementById('editor').value;
            const formatted = await this.madolaWrapper.format(code, 1);

            if (formatted) {
                document.getElementById('editor').value = formatted;
                this.hasUnsavedChanges = true;
                this.updateTitle();
                this.addMessage('success', 'Code formatted successfully');
            } else {
                this.addMessage('error', 'Formatting failed');
            }
        } catch (error) {
            this.addMessage('error', `Formatting error: ${error.message}`);
        }
    }

    async runCode() {
        if (!this.madolaWrapper || !this.madolaWrapper.isReady()) {
            this.addMessage('warning', 'WASM module not available. Showing demo output.');
            await this.showFallbackOutput();
            return;
        }

        try {
            const code = document.getElementById('editor').value.trim();
            if (!code) {
                document.getElementById('output').innerHTML = '<div class="output-placeholder"><p>Enter your MADOLA code to see the output here.</p></div>';
                return;
            }

            this.addMessage('info', 'Running MADOLA code...');

            // First, try to format the code to get markdown
            const markdownOutput = await this.madolaWrapper.format(code, 1);

            if (markdownOutput) {
                // Render the markdown with MathJax
                await this.renderMarkdown(markdownOutput);
                this.addMessage('success', 'Code executed and formatted successfully');
            } else {
                // If formatting fails, try evaluation
                const evaluated = await this.madolaWrapper.evaluate(code);

                if (evaluated) {
                    const evalResult = JSON.parse(evaluated);
                    if (evalResult.success && evalResult.outputs) {
                        const output = `<div class="markdown-content"><h3>Execution Result</h3><pre><code>${evalResult.outputs.join('\\n')}</code></pre></div>`;
                        document.getElementById('output').innerHTML = output;
                        this.addMessage('success', 'Code executed successfully');
                    } else if (evalResult.error) {
                        this.addMessage('error', evalResult.error);
                        document.getElementById('output').innerHTML = `<div class="markdown-content"><div class="message error">${evalResult.error}</div></div>`;
                    }
                } else {
                    this.addMessage('error', 'Execution failed');
                }
            }
        } catch (error) {
            this.addMessage('error', `Execution error: ${error.message}`);
        }
    }

    async showFallbackOutput() {
        const code = document.getElementById('editor').value.trim();
        if (!code) {
            document.getElementById('output').innerHTML = '<div class="output-placeholder"><p>Enter your MADOLA code to see the output here.</p></div>';
            return;
        }

        // Create a demo output showing what the formatted result would look like
        const demoMarkdown = `## MADOLA Code Execution

**Input Code:**
\`\`\`madola
${code}
\`\`\`

**Demo Output:**
This is a demonstration of what MADOLA output would look like.

**Mathematical Expression:**
$$f(x) = x^2 + 2x + 1$$

**Note:** WASM module not loaded. This is placeholder content.
To see actual results, ensure the WASM module loads properly.`;

        await this.renderMarkdown(demoMarkdown);
        this.addMessage('info', 'Showing demo output (WASM not available)');
    }

    async renderMarkdown(markdown) {
        try {
            // Use marked to convert markdown to HTML
            const html = marked.parse(markdown);

            // Create a container with the markdown content class
            const container = document.createElement('div');
            container.className = 'markdown-content';
            container.innerHTML = html;

            // Update output
            document.getElementById('output').innerHTML = '';
            document.getElementById('output').appendChild(container);

            // Re-render MathJax
            if (window.MathJax && window.MathJax.typesetPromise) {
                await window.MathJax.typesetPromise([container]);
            }
        } catch (error) {
            this.addMessage('error', `Markdown rendering error: ${error.message}`);
        }
    }

    // Output operations
    copyOutput() {
        const output = document.getElementById('output');
        const text = output.textContent || output.innerText;

        navigator.clipboard.writeText(text).then(() => {
            this.addMessage('success', 'Output copied to clipboard');
        }).catch(() => {
            this.addMessage('error', 'Failed to copy output');
        });
    }

    downloadMarkdown() {
        const output = document.getElementById('output');
        const markdown = output.innerHTML; // Get the HTML content

        // Try to get the original markdown if available
        const filename = (this.currentFile || 'output').replace(/\\.clg$/, '.md');
        this.downloadFile(filename, markdown);
    }

    // UI operations
    addMessage(type, text) {
        const messagesContainer = document.getElementById('messages');
        const messageDiv = document.createElement('div');
        messageDiv.className = `message ${type}`;
        messageDiv.textContent = `[${new Date().toLocaleTimeString()}] ${text}`;

        messagesContainer.appendChild(messageDiv);
        messagesContainer.scrollTop = messagesContainer.scrollHeight;
    }

    clearMessages() {
        document.getElementById('messages').innerHTML = '';
    }

    showAbout() {
        document.getElementById('about-modal').classList.remove('hidden');
    }

    hideAbout() {
        document.getElementById('about-modal').classList.add('hidden');
    }

    updateTitle() {
        const title = this.currentFile ?
            `MADOLA - ${this.currentFile}${this.hasUnsavedChanges ? '*' : ''}` :
            `MADOLA${this.hasUnsavedChanges ? '*' : ''}`;

        document.title = title;

        // Update Electron window title if running in Electron
        if (this.isElectron && window.electronAPI) {
            window.electronAPI.updateTitle(title);
        }
    }

    handleKeyboard(event) {
        // Ctrl+N - New file
        if (event.ctrlKey && event.key === 'n') {
            event.preventDefault();
            this.newFile();
        }
        // Ctrl+O - Open file
        else if (event.ctrlKey && event.key === 'o') {
            event.preventDefault();
            this.openFile();
        }
        // Ctrl+S - Save file
        else if (event.ctrlKey && event.key === 's') {
            event.preventDefault();
            if (event.shiftKey) {
                this.saveAsFile();
            } else {
                this.saveFile();
            }
        }
        // F5 or Ctrl+R - Run code
        else if (event.key === 'F5' || (event.ctrlKey && event.key === 'r')) {
            event.preventDefault();
            this.runCode();
        }
        // Ctrl+Shift+F - Format code
        else if (event.ctrlKey && event.shiftKey && event.key === 'F') {
            event.preventDefault();
            this.formatCode();
        }
    }
}

// Initialize the application when DOM is loaded
document.addEventListener('DOMContentLoaded', () => {
    new MadolaApp();
});