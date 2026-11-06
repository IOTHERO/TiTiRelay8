// Serial console for the production line (115200 baud, one command per line). A unit is
// given its platform identity without opening the portal:
//   id <device_id>          secret <device secret>       server <host> [port] [tls]
//   api <https://hub...>    wifi <ssid> [password]       mode wifi|rs485
//   qr <label QR text>      serial <serial number>       (product label, kept on factory reset)
//   show                    save                         restart
//   factory yes             (settings back, identity and label kept)
//   wipe yes                (everything, identity and label included)
// Secrets and the claim code are never printed back.
#pragma once

namespace console {
void loop();
}
