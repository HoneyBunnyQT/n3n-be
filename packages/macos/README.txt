n3n-be for macOS (universal: Apple Silicon and Intel)
=====================================================

n3n-be (Bunny Edition) is a peer to peer VPN, a fork of n3n:
https://github.com/HoneyBunnyQT/n3n-be

What is in here
---------------

- n3n-be, the program, and the links n3n-be-edge and n3n-be-supernode to
  it, which start it as an edge or a supernode
- edge.conf.sample and supernode.conf.sample, configurations to start from
- n3n-be-ctl, the management client (a Python 3 script)
- LICENSES: n3n-be is free software under the GNU Affero GPL, version 3;
  the parts that came from n3n and n2n keep their GNU GPL, version 3
  (some version 2).  Its source is on the page above

Running an edge
---------------

macOS has a TUN device of its own, utun, which the edge uses with
tuntap.type = tun: nothing to install, nothing to allow in the system
settings.  It needs root, as any tunnel does:

    sudo ./n3n-be-edge -O tuntap.type=tun -c mynet -k secret \
        -l sn.example.org:7654 -a 10.0.0.5/24 start

or with a config file (from edge.conf.sample, "type = tun" in its
[tuntap] section): sudo ./n3n-be-edge start edge.conf.  The edge takes the
next free utun device, puts the address on it and routes the community's
subnet into it.  The default, tuntap.type = tap, needs a third-party
kernel extension on macOS.

"./n3n-be-edge help config" lists the options; the documentation is on
the project's page.  n3n-be up to 3.4.8 had n3n's names - n3n-edge,
n3nctl, its configuration in /etc/n3n -, now its own, so that it can sit
next to n3n: its configuration is in /etc/n3n-be.

The programs are not notarized by Apple.  Downloaded with a browser, they
carry the quarantine attribute, and macOS refuses to start them; this
takes it off:

    xattr -dr com.apple.quarantine n3n-be-*-macos-universal
