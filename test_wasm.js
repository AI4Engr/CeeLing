const MADOLA = require('./dist/madola.js');

async function testMADOLA() {
    console.log('Testing MADOLA WASM API...');

    try {
        // Initialize WASM module
        const madola = await MADOLA();
        console.log('✅ WASM module loaded successfully');

        // Test parse_madola
        const parseResult = madola.ccall('parse_madola', 'number', ['string'], ['x1 := 42; print(x1);']);
        if (parseResult !== 0) {
            const parsed = madola.UTF8ToString(parseResult);
            console.log('✅ parse_madola result:', parsed);
            madola.ccall('free_result', null, ['number'], [parseResult]);
        } else {
            console.log('❌ parse_madola failed');
        }

        // Test evaluate_madola
        const evalResult = madola.ccall('evaluate_madola', 'number', ['string'], ['x1 := 42; print(x1);']);
        if (evalResult !== 0) {
            const evaluated = madola.UTF8ToString(evalResult);
            console.log('✅ evaluate_madola result:', evaluated);
            madola.ccall('free_result', null, ['number'], [evalResult]);
        } else {
            console.log('❌ evaluate_madola failed');
        }

        // Test format_madola
        const formatResult = madola.ccall('format_madola', 'number', ['string', 'number'], ['x1 := 42; print(x1);', 1]);
        if (formatResult !== 0) {
            const formatted = madola.UTF8ToString(formatResult);
            console.log('✅ format_madola result:');
            console.log(formatted);
            madola.ccall('free_result', null, ['number'], [formatResult]);
        } else {
            console.log('❌ format_madola failed');
        }

        console.log('\n🎉 All WASM API tests passed!');

    } catch (error) {
        console.error('❌ Error testing MADOLA WASM:', error);
    }
}

testMADOLA();