# Firmar una actualización de XCloud4

La aplicación comprueba una firma ECDSA P-256 sobre los bytes exactos del
manifiesto JSON. La clave pública de release se identifica por el SHA-256
de su SPKI DER
`2663584d81c0fa6a05bf0cbe878c286902fba284ea5dad5cbc0101b268203595`.
Cada paquete incluye su `APP_VER` y su versión de producto en
`PARAM.SFO`. Ambas versiones deben superar las de la aplicación abierta.

## Clave de release

Los siguientes comandos son para el dueño de la clave. Si la clave de
release ya existe, conserva ese par. Un par nuevo requiere una compilación
con su nueva clave pública antes de poder firmar actualizaciones para ella.

Desde PowerShell con Git para Windows, usa una ruta privada fuera del
repositorio y fuera de `work/`.

```powershell
$openssl = 'C:/Program Files/Git/usr/bin/openssl.exe'
$priv = 'D:/Llaves/XCloud4-update-release.pem'
$pub = 'D:/Llaves/XCloud4-update-public.pem'
if (Test-Path -LiteralPath $priv) { throw 'La clave privada ya existe' }
& $openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -aes-256-cbc -out $priv
if ($LASTEXITCODE) { throw 'Falló la generación de la clave' }
& $openssl pkey -in $priv -pubout -out $pub
if ($LASTEXITCODE) { throw 'Falló la exportación pública' }
```

La clave privada permanece bajo control del dueño. Guárdala en una
ubicación protegida y exclúyela del repositorio, de `work/`, de GitHub y
de la consola. El único archivo PEM que necesita el build es el público.

## Generar, firmar y comprobar

Genera el manifiesto con el nombre exacto del canal. La herramienta lee
el Title ID, APP_VER y la versión del PKG. También calcula el tamaño y
SHA-256 sin extraer su contenido.

```powershell
$pkg = 'XCloud4-0.7.100-lab-prueba-51b.pkg'
$json = 'xcloud4-update-lab.json'
if (!(Test-Path -LiteralPath $json)) {
    py -3 tools/make_update_manifest.py $pkg --output $json
    if ($LASTEXITCODE) { throw 'No se pudo generar el manifiesto' }
}
$manifest = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
$package = Get-Item -LiteralPath $pkg
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $pkg).Hash
if ($manifest.pkg_name -cne $package.Name -or $manifest.pkg_size -ne $package.Length -or
    $manifest.pkg_sha256 -ine $sha) { throw 'El manifiesto no coincide con el PKG' }
& $openssl dgst -sha256 -sign $priv -out "$json.sig" $json
if ($LASTEXITCODE) { throw 'No se pudo firmar el manifiesto' }
& $openssl dgst -sha256 -verify $pub -signature "$json.sig" $json
if ($LASTEXITCODE) { throw 'La firma no coincide' }
```

OpenSSL debe responder `Verified OK`. Conserva el nombre exacto del PKG y
los bytes del JSON firmado. Editar el JSON, cambiar saltos de línea o
añadir una marca de codificación invalida la firma.

| Canal | Archivos junto al PKG en la release |
| --- | --- |
| Pública | `xcloud4-update-public.json` y `xcloud4-update-public.json.sig` |
| Lab | `xcloud4-update-lab.json` y `xcloud4-update-lab.json.sig` |

Para la prueba local del Lab, coloca el PKG, el JSON firmado y su
`.json.sig` en `/data/pkg/`. Selecciona «Actualizar desde /data/pkg» y
confirma. La validación del archivo y de la firma es la misma que para una
descarga de GitHub.

El formato de la tabla CNT y de la entrada PARAM.SFO se contrastó con
LibOrbisPkg. [Tabla del PKG](https://github.com/maxton/LibOrbisPkg/blob/master/PS4PKG.bt) ·
[Formato SFO](https://github.com/maxton/LibOrbisPkg/blob/master/LibOrbisPkg/SFO/ParamSfo.cs).
