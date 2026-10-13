Throwaway certificates for `tests/test_online_hd.py` (TrustTests). They secure nothing: the
private key is here on purpose, and only a test server on 127.0.0.1 uses it.

`make_certs.sh` (MSYS2, needs `openssl`) made them:

- `root.pem`: a self-signed root.
- `mid_expired.pem`, `mid_valid.pem`: one intermediate authority, same key and name, certified
  twice by the root: an old certificate that expired in 2025 and its valid replacement.
- `server_chain.pem`, `leaf.key`: a server certificate for 127.0.0.1 and `localhost`, followed
  by the valid intermediate; and its key.

This is the shape of the chain thehuntersdream.com is on (Let's Encrypt reissued its
cross-signed roots), and of the failure the tests guard against: a PC whose trust store still
holds the expired copy.
