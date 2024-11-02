import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const __filename = url.fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const obj = JSON.parse(
  fs.readFileSync(path.join(__dirname, 'package.json'), 'utf8'),
);

obj.scripts = (({ build }) => {
  return { build };
})(obj.scripts);

obj.devDependencies = (({
  esbuild,
  'esbuild-sass-plugin': esbuild_saas_plugin,
}) => {
  return { esbuild, 'esbuild-sass-plugin': esbuild_saas_plugin };
})(obj.devDependencies);

// the replace() call adds a trailing newline if it doesn't exist
fs.writeFileSync(
  path.join(__dirname, 'package.json.buildonly'),
  JSON.stringify(obj, null, 2).replace(/^(.*?[^\n])$/s, '$1\n'),
);
