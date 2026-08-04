/**
 * list-printers.js
 * Run with: node list-printers.js
 *
 * Lists all printers currently installed on this Windows machine.
 * Copy the exact name of your Deli printer into PRINTER_NAME= in .env
 */
const { execSync } = require('child_process');

try {
    const output = execSync(
        'powershell -NoProfile -Command "Get-Printer | Select-Object Name, Default, PrinterStatus | Format-Table -AutoSize"',
        { encoding: 'utf8', timeout: 10000 }
    );

    console.log('\n==============================');
    console.log('  INSTALLED PRINTERS ON THIS PC');
    console.log('==============================');
    console.log(output.trim());
    console.log('==============================');
    console.log('\n  Copy the exact printer Name above into your .env file:');
    console.log('  PRINTER_NAME=<paste name here>\n');
} catch (err) {
    console.error('\n[ERROR] Could not list printers:', err.message);
}
