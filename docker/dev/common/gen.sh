set -eu

: $TOP
: $GFDOCKER_PRIMARY_USER
: $GFDOCKER_NUM_GFMDS
: $GFDOCKER_NUM_GFSDS
: $GFDOCKER_NUM_CLIENTS
: $GFDOCKER_IP_VERSION
: $GFDOCKER_SUBNET
: $GFDOCKER_START_HOST_ADDR
: $GFDOCKER_HOSTNAME_PREFIX_GFMD
: $GFDOCKER_HOSTNAME_PREFIX_GFSD
: $GFDOCKER_HOSTNAME_PREFIX_CLIENT
: $GFDOCKER_AUTH_TYPE

gen_gfservicerc() {
  cat <<EOF
# This file was automatically generated.

LOGNAME=${GFDOCKER_PRIMARY_USER}
EOF

  for i in $(seq 1 "$GFDOCKER_NUM_GFMDS"); do
    gfmd="${GFDOCKER_HOSTNAME_PREFIX_GFMD}${i}"
    cat <<EOF


##
## gfmd ${i}
##
gfmd${i}=${gfmd}
${gfmd}_CONFIG_GFARM_OPTIONS="-r -X -A \$LOGNAME -h \$gfmd${i} -a gsi_auth -D /O=Grid/OU=GlobusTest/OU=GlobusSimpleCA/CN=${GFDOCKER_PRIMARY_USER}"
gfmd${i}_AUTH_TYPE=${GFDOCKER_AUTH_TYPE}
EOF
  done

  for i in $(seq 1 "$GFDOCKER_NUM_GFSDS"); do
    gfsd="${GFDOCKER_HOSTNAME_PREFIX_GFSD}${i}"
    cat <<EOF


##
## gfsd ${i}
##
gfsd${i}=${gfsd}
gfsd${i}_CONFIG_GFSD_OPTIONS="-h \$gfsd${i} -l \$gfsd${i} -a docker"
gfsd${i}_AUTH_TYPE=${GFDOCKER_AUTH_TYPE}
EOF
  done

  for i in $(seq 1 "$GFDOCKER_NUM_CLIENTS"); do
    client="${GFDOCKER_HOSTNAME_PREFIX_CLIENT}${i}"
    cat <<EOF


##
## client ${i}
##
client${i}=${client}
EOF
  done
}

gen_gfservicerc > "${TOP}/docker/dev/common/rc.gfservice"
"${TOP}/docker/dev/common/gen_docker_compose_conf.py" \
  > "${TOP}/docker/dev/docker-compose.yml"
