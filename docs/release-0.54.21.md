# Navidog 0.54.21

- Added a Dameng SSL tab for the official JDBC certificate directory and keystore password, including optional encrypted local credential storage.
- Dameng sessions opened with SSL enabled must report an authenticated encryption mode after connection establishment. Navidog closes sessions that report plaintext or another unaccepted mode.
- Dameng SSL can be used on a direct connection or together with the SSH tunnel.
