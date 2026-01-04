# Ideas for future work on the pixlAnalyzer

radio chip: nrf52832, no PMIC.

The radio covers 2.4GHz ISM band, which is commonly used for various wireless communication protocols such as Bluetooth, Wi-Fi, and Zigbee.

Right now it only measures RSSI for spectrum analysis, on the range of 2400-2487MHz, with 1MHz steps.
(128 pixel display, so on average 1,45 pixels per frequency, some frequencies will have 2 pixels, some 1 pixel)

This functionality can be expanded in several ways:

1. **Signal identification**: Implement decoding for common protocols like Bluetooth Low Energy (BLE), IEEE 802.15.4 (Zigbee, Thread,..), or Wi-Fi. This would allow the device to not only detect signals but also identify the data being transmitted. This seems hard to do, as the nrf52832 is tailored for BLE, and nothing else. An aborted tentative to do this is in the "sniffer" branch.

Some shortcomings that can be improved (and have not been done yet):

1. better voltage calibration. On my OLED based device: 2.4V becomes 2.5V on screen, and 4V becomes 4.15V on screen.

And why not move to the latest SDK version while at it?
