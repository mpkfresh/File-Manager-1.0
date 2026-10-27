// Build-time raster export only; the application renders the SVG directly with Qt.
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const sharp = require(require.resolve('sharp', {
    paths: [path.join(root, 'out/branding-tools'), __dirname],
}));
sharp(path.join(root, 'resources/branding/app-logo.svg'), { density: 192 })
    .resize({ width: 2048, height: 2048, fit: 'inside' })
    .png()
    .toFile(path.join(root, 'resources/branding/app-logo.png'))
    .catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
