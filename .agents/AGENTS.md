# Project-Scoped Rules for RV06 SDK AI Agents

## Mandatory Project Status Maintenance
1. **Always Maintain `PROJECT_STATUS.md`**: Whenever any architectural modification, hardware pin mapping change, CMSV6 protocol fix, version bump, or firmware build occurs, the AI assistant MUST immediately update `/home/vunl/RV06_03_Linux_SDK/PROJECT_STATUS.md` with the new state (including current version, SHA256 checksum, IP history, protocol specs, and build results).
2. **Proper Firmware Packaging Sequence**:
   - Always execute `./build.sh app` after making changes to application source code before executing `./build.sh firmware`.
   - Running only `./build.sh firmware` without `./build.sh app` will NOT update the binaries inside the rootfs/image.
3. **Decoupled Architecture Enforcement**:
   - `acc_driver.h / acc_driver.c` must remain completely independent of RFID UART frames or serial protocols.
   - `rfid_driver.h / rfid_driver.c` must remain focused on RFID serial frames and IO channel states.
