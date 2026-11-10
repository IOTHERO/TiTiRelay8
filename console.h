// Serial console for the production line (115200 baud, one command per line). A unit is
// given its platform identity without opening the portal:
//   id <device_id>          secret <device secret>       server <host> [port] [tls]
//   api <https://hub...>    wifi <ssid> [password]       mode wifi|rs485
//   qr <label QR text>      serial <serial number>       (product label)
//   show                    save                         restart
//   factory yes | wipe yes  erase everything, identity and label included, so the board can be
//                           provisioned again (hub-provision). The customer's factory reset
//                           (portal, config switch) keeps the identity and the label.
// Same protocol as the hub's hub-provision (hub/provision.py). Secrets and the claim code are
// never printed back.
#pragma once

namespace console {
void loop();
}
