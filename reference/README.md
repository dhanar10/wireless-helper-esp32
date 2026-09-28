Terminal 1:
sudo python3 bt_agent.py

Terminal 2: 
sudo ./sdp_clean

Terminal 3:
sudo bluetoothctl power on 
sudo bluetoothctl discoverable on 
sudo bluetoothctl pairable on
sudo python3 bt_handshake.py
