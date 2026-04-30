```mermaid
flowchart LR
    subgraph Power
        BATT["3S LiPo Battery<br/>11.1V 5200mAh"]
        CAP["100µF Capacitor (35V)<br/>Spike Suppression"]
    end

    subgraph Control
        ESP["ESP32<br/>BLE + Control Logic"]
    end

    subgraph Driver
        DRV["DRV8825 Stepper Driver"]
    end

    subgraph Actuator
        MOTOR["Stepper Motor"]
    end

    BATT -->|VMOT| CAP --> DRV
    ESP -->|3.3V Logic| DRV
    ESP -->|STEP| DRV
    ESP -->|DIR| DRV
    ESP -->|ENABLE| DRV
    DRV --> MOTOR

    GND["Common Ground"]
    SLP["SLEEP & RESET tied HIGH"]

    ESP --- GND
    DRV --- GND
    DRV --- SLP
```
