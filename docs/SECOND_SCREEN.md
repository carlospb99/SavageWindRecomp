# Segunda pantalla

Panel web opcional y de solo lectura.

- Options > Gameplay > Second screen.
- Navegador local: http://127.0.0.1:8765
- WebSocket: /ws. JSON: /api/state.
- Desactivada por defecto. Puerto TCP fijo 8765.
- La opción de red local está desactivada por defecto; al activarla escucha en todas las interfaces. Úsala solo en una red de confianza.
- El hilo de red sirve una instantánea sincronizada; no lee memoria del juego.
- Incluye posición, resistencia, ubicación/región, hora cuando está disponible y código de clima.
- No publica corazones ni rupias: aún no hay lectores independientes verificados.
- El plano X/Z es esquemático, no cartografía de Hyrule ni un recurso extraído de Nintendo.
