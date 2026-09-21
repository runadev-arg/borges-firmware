# Borges 1.6.0-borges.1

## Base y compatibilidad

CrossPoint 1.6.0, commit `54337e6`, con FreeInk SDK `cb9167d541c0f6e9d57cf8eae1f564a939883ecc` y los parches de seguridad de Borges. El SDK se mantiene incluido en el repositorio, como en Borges 1.4.2-borges.2. Se preservan su verificación de certificados y nombres TLS, conexión por IP con SNI, resolución DNS mediante sockets y diagnóstico de errores. Se incorporan los límites de tiempo corregidos y la solicitud de registros TLS de 2 KiB de upstream.

La release publica el binario ESP32-C3 para X4/X3. No sirve para los modelos ESP32-S3. La compatibilidad X3 está en la compilación; no se probó físicamente ese modelo.

## Recuperación y rendimiento

- «Traer última posición» utiliza `/api/sync/v2/progress/positions`. El backend perdió esa ruta entre despliegues; se restauró en main con aislamiento por cuenta y recuperación de registros anteriores. No era un problema del porcentaje 37,76 % registrado por el Kindle.
- La recuperación consulta exclusivamente el libro abierto y no avanza el cursor de replicación ni vuelve a fechar posiciones antiguas. Aplicar una posición requiere consentimiento y persistencia local.
- El lector ya no se cierra cinco segundos después de mostrar una página para sincronizar y reabrirse. Tampoco intenta reconectar Wi-Fi al abrir cada libro. Conserva la cola durable, las acciones manuales, la sincronización fuera del lector cuando hay Wi-Fi y un intento al suspender.
- La posición remota reemplaza también el desplazamiento textual local. Se conserva el guardado atómico de archivos de progreso y sus respaldos ante cortes de alimentación.
- Se preservan notas y conflictos de marcadores en el menú nuevo. Su almacenamiento de filas admite las acciones adicionales de Borges.
- KOSync reconoce respuestas exitosas 2xx y ausencia de progreso por HTTP 204, 404 o 200 `{}`. Los errores del menú incluyen el estado HTTP cuando existe; los identificadores internos no se muestran como nombres de dispositivos.

El mecanismo de mejora principal evita solicitudes TLS bloqueantes y recarga de EPUB durante la lectura. No se publica un porcentaje de aceleración ni una medición de batería: falta la comprobación física del X4.

## Comparación con CrossPoint Sync

Fuentes: [servicio oficial](https://sync.crosspointreader.com/), [API oficial](https://github.com/crosspoint-reader/crosspoint-sync/blob/main/docs/API.md), [CrossPoint 1.6.0](https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/1.6.0).

El servicio oficial ofrece KOSync, posiciones por dispositivo, referencias de contenido y sincronización incremental con respuestas acotadas. Sus conectores corren en el servidor, una buena decisión para un ESP32. Para progreso elige el timestamp más reciente; para anotaciones usa última escritura y tombstones.

Se mantiene Borges: ya dispone de cuentas, cola durable, revisiones, conflictos y consentimiento antes de saltar de posición. Sustituirlo obligaría a migrar identidades y semánticas sin resolver el endpoint ausente. Se adoptan las mejoras del motor y compatibilidad KOSync, sin enviar bibliotecas ni credenciales al servicio externo. Los lotes siguen acotados a la memoria disponible; aumentar su tamaño sin medir el heap del dispositivo sería prematuro.

## Validación y límites

- Backend: 100 pruebas unitarias y 40 pruebas con PostgreSQL 17 nativo; ruta comprobada dentro del contenedor desplegado. Consulta del libro afectado en producción: 23 ms en una transacción de solo lectura.
- Firmware: suite nativa de 252 pruebas, incluyendo colas durables, migración, recuperación, guardado atómico, credenciales, KOSync y parsers.
- Compilación: `pio run -e borges_release` para ESP32-C3. Integración y publicación sujetas a compilación exitosa.
- El token antiguo guardado para una sonda HTTP devolvió 401; no se rotaron credenciales del usuario para realizar la prueba.
- No se flasheó automáticamente un dispositivo inaccesible. Validación física pendiente: actualizar, abrir un libro, pasar páginas, esperar más de cinco segundos, traer la posición desde el menú, confirmar y comprobar que persiste tras suspender y reabrir.

La versión anterior permanece en el canal `known-good`; publicar una compilación nueva no constituye validación física.
