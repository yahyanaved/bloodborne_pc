#!/usr/bin/env bash
# A small certificate hierarchy shaped like the one thehuntersdream.com is on:
#   root (self-signed, valid)  ->  mid (cross-signed twice with ONE key and name: an old
#   certificate that has expired, and its valid replacement)  ->  leaf for 127.0.0.1
# The server sends leaf + the valid mid. A client whose trust store still holds the expired
# mid (as a Windows "Intermediate Certification Authorities" store can) is the case to test.
# Usage (MSYS2): bash make_pki.sh <output dir>
set -euo pipefail
out=$1
mkdir -p "$out" && cd "$out"
# The native openssl takes "/O=.../CN=..." as it is: no MSYS path conversion of arguments.
export MSYS2_ARG_CONV_EXCL='*'
cat > ext.cnf <<'EOF'
[req]
distinguished_name = dn
prompt = no
[dn]
CN = unused
[v3_ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[v3_leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = IP:127.0.0.1, DNS:localhost
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF
gen() { openssl ecparam -name prime256v1 -genkey -noout -out "$1"; }
gen root.key; gen mid.key; gen leaf.key
openssl req -new -x509 -key root.key -subj "/O=bbport test/CN=Test Root X1" -days 36500 \
    -extensions v3_ca -config ext.cnf -out root.pem
openssl req -new -key mid.key -subj "/O=bbport test/CN=Test Root X2" -config ext.cnf -out mid.csr
openssl x509 -req -in mid.csr -CA root.pem -CAkey root.key -set_serial 2 \
    -not_before 20200904000000Z -not_after 20250915160000Z \
    -extfile ext.cnf -extensions v3_ca -out mid_expired.pem
openssl x509 -req -in mid.csr -CA root.pem -CAkey root.key -set_serial 3 -days 36500 \
    -extfile ext.cnf -extensions v3_ca -out mid_valid.pem
openssl req -new -key leaf.key -subj "/CN=127.0.0.1" -config ext.cnf -out leaf.csr
openssl x509 -req -in leaf.csr -CA mid_valid.pem -CAkey mid.key -set_serial 4 -days 36500 \
    -extfile ext.cnf -extensions v3_leaf -out leaf.pem
cat leaf.pem mid_valid.pem > server_chain.pem
openssl version
for c in root mid_expired mid_valid leaf; do
    printf '%-12s ' "$c"; openssl x509 -in "$c.pem" -noout -subject -enddate | tr '\n' ' '; echo
done
