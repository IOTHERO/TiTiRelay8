// Fallback page when the LittleFS image (data/index.html) has not been uploaded yet.
// The real UI lives in flash: Arduino IDE -> Ctrl+Shift+P -> "Upload LittleFS to Pico/ESP8266/ESP32".
#pragma once

static const char FALLBACK_HTML[] = R"HTML(<!doctype html>
<html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TiTi Relay-8</title><style>body{font-family:Tahoma,system-ui,sans-serif;background:#0a0c11;color:#eef0f6;display:grid;place-items:center;min-height:100vh;margin:0;padding:20px}
.b{max-width:440px;padding:28px;border:1px solid rgba(255,255,255,.1);border-radius:20px;background:rgba(255,255,255,.04)}h1{color:#d9b46a;font-size:20px}
p{color:#9aa3b5;line-height:1.8}code{direction:ltr;display:inline-block;background:#000;padding:2px 6px;border-radius:6px}
input,button{font:inherit;margin-top:10px;width:100%;padding:10px;border-radius:10px;border:1px solid #333;background:#111;color:#eee}button{background:#d9b46a;color:#1a1307;border:0;font-weight:700}</style></head>
<body><div class="b"><h1>TiTi Relay-8</h1>
<p>فایل‌های رابط کاربری (LittleFS) روی حافظه نیست. پوشهٔ <code>data</code> را با «Upload LittleFS» در Arduino IDE بارگذاری کنید، یا فایل <code>littlefs.bin</code> را این‌جا نصب کنید.</p>
<p dir="ltr" style="text-align:left">The web UI (LittleFS) is missing. Upload the <code>data</code> folder with "Upload LittleFS", or install a LittleFS image below.</p>
<input id="p" type="password" placeholder="admin password"><input id="f" type="file" accept=".bin"><button id="u">Upload filesystem image</button><p id="s"></p></div>
<script>
document.getElementById("u").onclick=async()=>{const s=document.getElementById("s");
const r=await fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({password:document.getElementById("p").value})});
if(!r.ok){s.textContent="password?";return}const tk=(await r.json()).token;const f=document.getElementById("f").files[0];if(!f)return;
const fd=new FormData();fd.append("fs",f,f.name);s.textContent="...";
const x=await fetch("/api/ota?fs=1",{method:"POST",headers:{"X-Auth":tk},body:fd});s.textContent=x.ok?"OK, restarting":"failed";if(x.ok)setTimeout(()=>location.reload(),9000)};
</script></body></html>
)HTML";
