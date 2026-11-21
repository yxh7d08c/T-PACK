# T-PACK: TD-Partitioning-based Access and Conduct Keep

## 1 Features and Boundaries

L1/SVSM serves as the management domain M, L2-1 as the user domain U, and L2-2 as the compute domain C. The two L2 domains are peer execution domains. The management domain manages inter-domain memory access through Intel TDX TD Partitioning page aliases and access attributes, and handles guest calls, device access, and interrupts.

## 2 Build and Run

~~~bash
cd /home/hyc/temp/TEEcapsule
cat manifests/external-images.json
# First, add two real root disk images and verify that the kernels and modules match
./scripts/build.sh qemu
./scripts/build.sh edk2

# Build SVSM only on the designated build machine; do not run make automatically on this machine
./scripts/build.sh svsm-help
# Run make svsm.bin on the build machine, then copy the artifact back
# td-partitioning-svsm/svsm.bin

# Build the modules and init for the actual guest kernel, then repackage them
KDIR=/path/to/matching/guest/kernel/build ./scripts/build.sh guest
# Package only existing matching payloads:
# ./scripts/build.sh initrd

./scripts/preflight.sh
./scripts/run.sh
~~~

### 5.1 Serial Consoles and Verification

~~~bash
# In other host terminals, from this directory
./connect_l2_1.sh
./connect_l2_2.sh
./scripts/verify_logs.sh
~~~

### 5.2 SSH and C Channel

U networking requires QEMU to have slirp enabled and the root disk to contain sshd, a user account, and a valid public key. By default, port forwarding listens on 127.0.0.1:10021 on the host and is not exposed directly to external networks. Users can establish a tunnel through the host:

~~~bash
# On the user's machine
ssh -N -L 10021:127.0.0.1:10021 host-user@tdx-host
# In another terminal on the user's machine
ssh -p 10021 guest-user@127.0.0.1
~~~

No default password or personal private key is provided. Create your own user key and non-root guest account, verify authorized_keys, sshd, and firewall settings, and do not reuse server administration keys.

~~~bash
HYC3_L2_2_NET=0 HYC3_EXPOSE_L2_2_SERIAL=0 ./scripts/run.sh
~~~
