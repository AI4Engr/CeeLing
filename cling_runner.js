#!/usr/bin/env node

const CLING = require('./dist/cling.js');
const fs = require('fs');
const path = require('path');

async function main() {
    try {
        const args = process.argv.slice(2);
        if (args.length < 1) {
            console.error('Usage: node cling_runner.js <input.cl> [--output <file>]');
            process.exit(1);
        }

        const inputFile = args[0];
        const outputIndex = args.indexOf('--output');
        const outputFile = outputIndex !== -1 ? args[outputIndex + 1] : null;

        // Initialize WASM module
        const cling = await CLING();

        // Read input file
        const testContent = fs.readFileSync(inputFile, 'utf8');

        // Evaluation mode
        const evalResultPtr = cling.ccall('evaluate_cling', 'number', ['string'], [testContent]);
        let result;
        if (evalResultPtr !== 0) {
            const evaluated = cling.UTF8ToString(evalResultPtr);
            cling.ccall('free_result', null, ['number'], [evalResultPtr]);

            // Parse the JSON response and extract outputs
            const evalResult = JSON.parse(evaluated);
            if (evalResult.success && evalResult.outputs) {
                result = 'Execution completed successfully\nOutput:\n  ' + evalResult.outputs.join('\n');
            } else if (evalResult.error) {
                result = evalResult.error;
            } else {
                result = 'Error: No output or error from evaluation';
            }
        } else {
            result = 'Error: WASM evaluation failed';
        }

        // Output result
        if (outputFile) {
            // Normalize line endings to match platform (Windows uses \r\n)
            const normalizedResult = result.replace(/\r?\n/g, '\r\n');
            fs.writeFileSync(outputFile, normalizedResult, 'utf8');
        } else {
            // Normalize line endings to match platform on console output too
            const normalizedResult = process.platform === 'win32' ?
                result.replace(/\r?\n/g, '\r\n') : result;
            process.stdout.write(normalizedResult + (process.platform === 'win32' ? '\r\n' : '\n'));
        }

    } catch (error) {
        console.error('Error:', error.message);
        process.exit(1);
    }
}

main();

