#!/usr/bin/env node

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = 8080;

const mimeTypes = {
    '.html': 'text/html',
    '.js': 'application/javascript',
    '.wasm': 'application/wasm',
    '.css': 'text/css',
    '.json': 'application/json',
    '.md': 'text/markdown',
    '.cl': 'text/plain'
};

const server = http.createServer((req, res) => {
    // Parse URL and route appropriately
    let filePath = req.url;
    
    console.log(`[${new Date().toISOString()}] ${req.method} ${req.url}`);
    
    // Route root to web/index.html for the main web app
    if (filePath === '/') {
        filePath = '/web/index.html';
    }
    // Route demo.html requests to the root demo
    else if (filePath === '/demo.html') {
        filePath = '/demo.html';
    }
    // Route web app assets to web directory
    else if (filePath.startsWith('/css/') || filePath.startsWith('/js/') || 
             (filePath.endsWith('.js') && !filePath.includes('serve.js')) || 
             filePath.endsWith('.wasm')) {
        // These are web assets, serve from web directory
        filePath = '/web' + filePath;
    }
    
    filePath = path.join(__dirname, filePath);

    // Get file extension
    const ext = path.extname(filePath);
    const contentType = mimeTypes[ext] || 'application/octet-stream';

    // Check if file exists
    fs.access(filePath, fs.constants.F_OK, (err) => {
        if (err) {
            console.log(`[404] File not found: ${filePath}`);
            res.writeHead(404, { 'Content-Type': 'text/plain' });
            res.end('404 Not Found');
            return;
        }

        // Set CORS headers for WASM and cross-origin requests
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
        res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
        
        // Set headers required for SharedArrayBuffer and WASM
        res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
        res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');

        // Read and serve file
        fs.readFile(filePath, (err, data) => {
            if (err) {
                console.log(`[500] Error reading file: ${filePath}`);
                res.writeHead(500, { 'Content-Type': 'text/plain' });
                res.end('500 Internal Server Error');
                return;
            }

            res.writeHead(200, { 'Content-Type': contentType });
            res.end(data);
            console.log(`[200] Served: ${filePath} (${contentType})`);
        });
    });
});

server.listen(PORT, () => {
    console.log(`🚀 MADOLA Demo Server running at http://localhost:${PORT}`);
    console.log(`📄 Main Web App: http://localhost:${PORT}/`);
    console.log(`📄 Simple Demo: http://localhost:${PORT}/demo.html`);
    console.log(`📁 Serving files from: ${__dirname}`);
});

