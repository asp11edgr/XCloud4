# Inventario de Cadenas TLS y Certificados Raíz - XCloud4

Este documento contiene el inventario completo de las cadenas TLS de los servicios utilizados por XCloud4,
así como las autoridades certificadoras raíz (Root CAs) y certificados de firma cruzada requeridos para
garantizar conectividad HTTPS segura en firmwares antiguos de PlayStation 4 (como 5.05, issue #3).

## 1. Origen del Almacén de Certificados Raíz

- **Origen:** Almacén de certificados raíz de Mozilla (extraído vía curl/certifi)
- **URL fijada:** `https://curl.se/ca/cacert-2024-09-24.pem`
- **SHA-256 del archivo de origen:** `189d3cf6d103185fba06d76c1af915263c6d42225481a1759e853b33ac857540`
- **Licencia:** Mozilla Public License 2.0 (MPL-2.0)

## 2. Certificados Raíz Empaquetados

| # | Nombre del Certificado Raíz | Algoritmo | SHA-256 Fingerprint |
|---|-----------------------------|-----------|---------------------|
| 1 | DigiCert Global Root G2 | RSA | `CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F` |
| 2 | DigiCert Global Root G3 | ECC | `31:AD:66:48:F8:10:41:38:C7:38:F3:9E:A4:32:01:33:39:3E:3A:18:CC:02:29:6E:F9:7C:2A:C9:EF:67:31:D0` |
| 3 | DigiCert Global Root CA | RSA | `43:48:A0:E9:44:4C:78:CB:26:5E:05:8D:5E:89:44:B4:D8:4F:96:62:BD:26:DB:25:7F:89:34:A4:43:C7:01:61` |
| 4 | Microsoft RSA Root Certificate Authority 2017 | RSA | `C7:41:F7:0F:4B:2A:8D:88:BF:2E:71:C1:41:22:EF:53:EF:10:EB:A0:CF:A5:E6:4C:FA:20:F4:18:85:30:73:E0` |
| 5 | Microsoft ECC Root Certificate Authority 2017 | ECC | `35:8D:F3:9D:76:4A:F9:E1:B7:66:E9:C9:72:DF:35:2E:E1:5C:FA:C2:27:AF:6A:D1:D7:0E:8E:4A:6E:DC:BA:02` |
| 6 | USERTrust RSA Certification Authority | RSA | `E7:93:C9:B0:2F:D8:AA:13:E2:1C:31:22:8A:CC:B0:81:19:64:3B:74:9C:89:89:64:B1:74:6D:46:C3:D4:CB:D2` |
| 7 | USERTrust ECC Certification Authority | ECC | `4F:F4:60:D5:4B:9C:86:DA:BF:BC:FC:57:12:E0:40:0D:2B:ED:3F:BC:4D:4F:BD:AA:86:E0:6A:DC:D2:A9:AD:7A` |
| 8 | Baltimore CyberTrust Root | RSA | `16:AF:57:A9:F6:76:B0:AB:12:60:95:AA:5E:BA:DE:F2:2A:B3:11:19:D6:44:AC:95:CD:4B:93:DB:F3:F2:6A:EB` |
| 9 | DigiCert High Assurance EV Root CA | RSA | `74:31:E5:F4:C3:C1:CE:46:90:77:4F:0B:61:E0:54:40:88:3B:A9:A0:1E:D0:0B:A6:AB:D7:80:6E:D3:B1:18:CF` |
| 10 | Sectigo Public Server Authentication Root E46 | ECC | `C9:0F:26:F0:FB:1B:40:18:B2:22:27:51:9B:5C:A2:B5:3E:2C:A5:B3:BE:5C:F1:8E:FE:1B:EF:47:38:0C:53:83` |
| 11 | Sectigo Public Server Authentication Root R46 | RSA | `7B:B6:47:A6:2A:EE:AC:88:BF:25:7A:A5:22:D0:1F:FE:A3:95:E0:AB:45:C7:3F:93:F6:56:54:EC:38:F2:5A:06` |

## 3. Cadenas TLS por Host de Servicio

### Host: `login.microsoftonline.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=Washington, L=Redmond, O=Microsoft Corporation, CN=stamp2.login.microsoftonline.com` | `C=US, O=DigiCert Inc, CN=DigiCert Global G2 TLS RSA SHA256 2020 CA1` |
| 1 | Raíz / Ancla | `C=US, O=DigiCert Inc, CN=DigiCert Global G2 TLS RSA SHA256 2020 CA1` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `login.live.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=Washington, L=Redmond, O=Microsoft Corporation, CN=login.live.com` | `C=US, O=DigiCert Inc, CN=DigiCert Global G2 TLS RSA SHA256 2020 CA1` |
| 1 | Raíz / Ancla | `C=US, O=DigiCert Inc, CN=DigiCert Global G2 TLS RSA SHA256 2020 CA1` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `user.auth.xboxlive.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=user.auth.xboxlive.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` |
| 2 | Raíz / Ancla | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `xsts.auth.xboxlive.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=ssl-xsts.auth.xboxlive.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` |
| 2 | Raíz / Ancla | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `xgpuweb.gssv-play-prod.xboxlive.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=gssv-play-prod.xboxlive.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` |
| 2 | Raíz / Ancla | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `xgpuwebf2p.gssv-play-prod.xboxlive.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=gssv-play-prod.xboxlive.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 10` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` |
| 2 | Raíz / Ancla | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Región Este de EE. UU.: `eus.core.gssv-play-prod.xboxlive.com`

*La prueba histórica usó la etiqueta equivocada `eastus`; el fallo de DNS no demuestra que el host correcto sea privado.*

### Región Europa occidental: `weu.core.gssv-play-prod.xboxlive.com`

*La prueba histórica usó la etiqueta equivocada `westeurope`; no validó el host abreviado.*

### Host: `displaycatalog.mp.microsoft.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=*.bigcatalog-frontdoor-prod.commerce.microsoft.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 02` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 RSA CA OCSP 02` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` |
| 2 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS RSA Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |
| 3 | Raíz / Ancla | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2` |

### Host: `store-images.s-microsoft.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `C=US, ST=WA, L=Redmond, O=Microsoft Corporation, CN=store-images.microsoft.com` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 ECC CA OCSP 02` |
| 1 | Intermedio | `C=US, O=Microsoft Corporation, CN=Microsoft TLS G2 ECC CA OCSP 02` | `C=US, O=Microsoft Corporation, CN=Microsoft TLS ECC Root G2` |
| 2 | Raíz / Ancla | `C=US, O=Microsoft Corporation, CN=Microsoft TLS ECC Root G2` | `C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G3` |

### Host: `api.github.com`

| Nivel | Tipo | Sujeto (Subject) | Emisor (Issuer) |
|-------|------|------------------|-----------------|
| 0 | Hoja (Leaf) | `CN=*.github.com` | `C=GB, O=Sectigo Limited, CN=Sectigo Public Server Authentication CA DV E36` |
| 1 | Intermedio | `C=GB, O=Sectigo Limited, CN=Sectigo Public Server Authentication CA DV E36` | `C=GB, O=Sectigo Limited, CN=Sectigo Public Server Authentication Root E46` |
| 2 | Raíz / Ancla | `C=GB, O=Sectigo Limited, CN=Sectigo Public Server Authentication Root E46` | `C=US, ST=New Jersey, L=Jersey City, O=The USERTRUST Network, CN=USERTrust ECC Certification Authority` |

### Hosts Regionales: `<región>.core.gssv-play-prod.xboxlive.com`

Los endpoints regionales usan etiquetas abreviadas como `mxc`, `eus`, `weu`,
`brs`, `uks` y `cia`. Por ejemplo, `mxc.core.gssv-play-prod.xboxlive.com` y
`eus.core.gssv-play-prod.xboxlive.com`
comparten la misma infraestructura PKI de Xbox Cloud Gaming que `xgpuweb.gssv-play-prod.xboxlive.com`:

- **Sujeto:** `CN=gssv-play-prod.xboxlive.com`
- **SANs incluidos:** `*.americas.gssv-play-prod.xboxlive.com`, `*.europe.gssv-play-prod.xboxlive.com`, `*.asiapacific.gssv-play-prod.xboxlive.com`, `*.core.gssv-play-prod.xboxlive.com`, `*.gssv-play-prod.xboxlive.com`
- **Emisor Intermedio:** `CN=Microsoft TLS G2 RSA CA OCSP 10`
- **Emisor Intermedio Superior:** `CN=Microsoft TLS RSA Root G2`
- **Raíz Ancla / Firma Cruzada:** `CN=DigiCert Global Root G2`

## 4. Nota sobre la Variante Experimental cabundle (Descartada)

La variante experimental `XCloud4-0.7.57-lab-cabundle.pkg` (asociada al flag de compilación `X4_CA_BUNDLE_ONLY=1`) fue evaluada y formalmente descartada por las siguientes razones técnicas:

1. **Incompatibilidad con el modelo de verificación de libSceSsl / sceHttps:** En la biblioteca TLS de PlayStation 4, omitir la bandera `KNOWN_CA` (`0x20u`) en `sceHttpsEnableOption` no elimina ni desactiva las raíces del sistema preinstaladas en el firmware. En su lugar, desactiva la comprobación estricta de que la cadena TLS termine obligatoriamente en una raíz de confianza conocida.
2. **Falso positivo de cobertura y degradación de seguridad:** Desactivar `KNOWN_CA` permite que cualquier certificado sin ancla confiable pueda considerarse válido bajo ciertas condiciones, lo que debilitaría la seguridad criptográfica del transporte HTTPS en lugar de simular con precisión un almacén antiguo. Además, la prueba de validación pasaría con o sin nuestro paquete de certificados, perdiendo valor probatorio.
3. **Estrategia histórica anterior a T31:** Se utilizaba la verificación estricta completa `X4_HTTPS_VERIFY_ALL` (`0xBDu`) tanto en la edición pública como en el laboratorio. Las raíces empaquetadas se registraban en el contexto HTTP mediante `sceHttpsLoadCert`, con reintento a las 7 raíces RSA cuando el firmware rechazaba las ECC. La T31 sustituye ese transporte como se describe a continuación; no vuelve a la variante `cabundle` descartada.

## 5. Transporte HTTPS propio — T31 (Lab 0.7.70 / pública 01.14)

Todo el HTTPS de ambos productos pasa por Mbed TLS 3.6.7, ya enlazado para DTLS,
sin cargar libSceHttp/libSceSsl ni recurrir a ellos si falla TLS. Se conservan la
validación de destinos, métodos y cabeceras, los plazos, la cancelación y los dos
canales públicos keep-alive. No se utiliza un proxy configurado en la consola.

Cada contexto HTTP analiza las **11 raíces DER** de este inventario. La cadena,
las fechas y el nombre del host se verifican obligatoriamente; SNI y comprobación
del nombre usan el mismo host validado. No hay pinning, modo inseguro ni opción
para desactivar la verificación. Un error de fecha pide revisar fecha y hora.

Se ofrecen TLS 1.2 y 1.3, con ECDHE y AES-GCM o ChaCha20-Poly1305 en TLS 1.2,
y las tres suites AEAD de TLS 1.3. No se ofrecen CBC, RSA estático, DHE ni MAC
SHA-1. Los grupos son X25519, secp256r1 y secp384r1. No se guardan tickets ni
sesiones TLS para reanudarlas. La entropía procede del RNG seguro nativo;
un fallo hace inutilizable el contexto, sin semilla alternativa.

La tabla de cadenas anterior es un inventario histórico, no una garantía de
disponibilidad ni de que un proveedor mantenga sus certificados. La fase A de
T31 distingue los hosts que completaron TLS de los nombres regionales que
no resolvieron; los resultados y sus límites se conservan en el diseño
de la entrega. La aceptación real en firmware 5.05 sigue pendiente de la prueba
del dueño en consola.
