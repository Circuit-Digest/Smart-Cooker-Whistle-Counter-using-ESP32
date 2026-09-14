# Smart Cooker Whistle Counter using ESP32

An IoT-based **Smart Pressure Cooker Whistle Counter & Alerting System** using **ESP32**, **MAX4466 Microphone Module**, **CircuitDigest Cloud**, and **WhatsApp API**.

Pressure cookers are indispensable in the kitchen, but keeping track of whistles manually is tedious and easy to forget. Missing a whistle count leads to burnt food, ruined utensils, and potential kitchen hazards. 

This smart device listens for sharp acoustic peak spikes caused by cooker whistles, counts them automatically in real time, and sends an instant WhatsApp alert notification to your mobile phone once your preset target whistle count is completed.

---

## 🌟 Features

- **Acoustic Whistle Peak Detection**: Continuous sampling of MAX4466 mic output over 20ms windows to calculate peak-to-peak voltage ($\Delta V$) and convert it into logarithmic decibels (dB).
- **CircuitDigest Cloud Dashboard Control**:
  - `analog-input-1`: Target whistle count set via dashboard slider (1 to 20 whistles).
  - `analog-input-2`: Start/Stop listening toggle switch.
  - `analog-input-3`: Live whistle count readback.
  - `analog-input-4`: Live sound decibel level (dB) updated in real time (~1 update/sec).
- **Instant WhatsApp Alerts**: Automatically dispatches a WhatsApp notification via CircuitDigest Cloud REST API (`www.circuitdigest.cloud:443`) when target whistle count is reached.
- **Echo & Double-Count Prevention**: Built-in 10-second cooldown window (`WHISTLE_COOLDOWN_MS`) and minimum whistle duration threshold (`MIN_WHISTLE_MS = 100ms`) prevent secondary echo spikes from double-counting.
- **Persistent State Reset & Robust Reconnection**: Resets state to 0/OFF on boot/reset to prevent stale values, and auto-restarts ESP32 on Wi-Fi loss.

---

## 📐 Detection Logic & Formula

### Peak-to-Peak Voltage Calculation
$$\Delta V = V_{\text{max}} - V_{\text{min}}$$

### Decibel (dB) Conversion
$$\text{dB} = 41.52 \cdot \log_{10}(\Delta V) + 64.02$$

### Whistle Detection Logic
1. **Whistle ON**: Sound level exceeds `WHISTLE_ON_DB` ($60.0 \text{ dB}$).
2. **Whistle OFF**: Sound drops below `WHISTLE_OFF_DB` ($50.0 \text{ dB}$).
3. **Validation**: The high-sound burst must stay above the threshold for at least `MIN_WHISTLE_MS` ($100 \text{ ms}$).
4. **Cooldown**: After a valid whistle is logged, a 10-second timer (`WHISTLE_COOLDOWN_MS = 10000 ms`) locks out further whistle increments.

---

## 🛠️ Hardware Components Required

| S.No | Component | Specification | Quantity |
| :---: | :--- | :--- | :---: |
| 1 | Microcontroller | ESP32 Dev Kit / DevKitC | 1 |
| 2 | Microphone Module | MAX4466 Electret Mic with Adjustable Gain | 1 |
| 3 | Power Source | 5V USB Power Supply / Power Bank | 1 |
| 4 | Wiring | Jumper Wires & Breadboard | 1 |

---

## 🔌 Circuit & Wiring Diagram

| MAX4466 Pin | ESP32 GPIO Pin | Description |
| :---: | :---: | :--- |
| **VCC** | **3.3V** | Power Supply |
| **GND** | **GND** | Ground |
| **OUT** | **GPIO 34** | Analog Input (ADC1_CH6) |

> [!NOTE]
> GPIO34 is an input-only ADC1 channel on the ESP32. Using ADC1 avoids Wi-Fi interference issues that can occur on ADC2 pins.

---

## ☁️ CircuitDigest Cloud & WhatsApp Setup

1. **Sign Up / Login**: Access [CircuitDigest Cloud](https://www.circuitdigest.cloud).
2. **Add New Device**: Name your device (e.g., `Cooker Whistle Monitoring and Alerting System`).
3. **Add Variables**:
   - `analog-input-1`: Slider (Target Whistle Count, Bidirectional)
   - `analog-input-2`: Toggle Switch (Arm/Disarm Listening, Bidirectional)
   - `analog-input-3`: Number/Gauge (Live Whistle Count, Input to Dashboard)
   - `analog-input-4`: Number/Gauge (Live dB Level, Input to Dashboard)
4. **WhatsApp API Integration**: Go to WhatsApp Notification settings in CircuitDigest Cloud, link your mobile number, verify via OTP, and retrieve your API Key & Device Credentials.

---

## 📁 Repository Structure

```
Smart-Cooker-Whistle-Counter-using-ESP32/
├── code/
├── src/
│   └── Smart_Cooker_Whistle_Counter.ino    # Complete ESP32 Arduino Sketch
├── LICENSE                                  # MIT License
└── README.md                                # Project Documentation
```

---

## ⚙️ Calibration & Troubleshooting

- **Microphone Trimmer Calibration**: Adjust the built-in potentiometer on the back of the MAX4466 module until room ambient sound reads around ~30–40 dB in a quiet room.
- **Stuck at 80 dB**: If quiet room readings stay around ~80 dB despite trimming, inspect wiring or try replacing the MAX4466 module.
- **Missed Whistles**: If pressure cooker whistles aren't detected, reduce `WHISTLE_ON_DB` (e.g., from 60.0 to 55.0 dB) in the code.
- **Wi-Fi Issues**: Ensure 2.4 GHz Wi-Fi credentials are accurately entered in the sketch.

---

## 📜 License

This project is licensed under the [MIT License](LICENSE).
