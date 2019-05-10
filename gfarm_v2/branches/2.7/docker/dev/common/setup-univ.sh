#! /bin/sh

# Usage (Dockerfile):
#
# ENV GFDOCKER_DIST_NAME <centos of opensuse>
#
# ARG GFDOCKER_USERNAME_PREFIX
# ARG GFDOCKER_PRIMARY_USER
# ARG GFDOCKER_NUM_GFMDS
# ARG GFDOCKER_NUM_GFSDS
# ARG GFDOCKER_NUM_USERS
# ARG GFDOCKER_HOSTNAME_PREFIX_GFMD
# ARG GFDOCKER_HOSTNAME_PREFIX_GFSD
#
# RUN for i in $(seq 1 "$GFDOCKER_NUM_USERS"); do \
# 		useradd -m -s /bin/bash -U \
# 			"${GFDOCKER_USERNAME_PREFIX}${i}"; \
# 	done
#
# # chown option does not use variables.
# # see https://github.com/moby/moby/issues/35018 for details.
# # XXX FIXME: use variable.
# # NOTE: Enable buildkit
# #COPY --chown=${GFDOCKER_PRIMARY_USER}:${GFDOCKER_PRIMARY_USER} \
# #	. "/home/${GFDOCKER_PRIMARY_USER}/gfarm"
# #COPY --chown=${GFDOCKER_PRIMARY_USER}:${GFDOCKER_PRIMARY_USER} \
# #	gfarm2fs "/home/${GFDOCKER_PRIMARY_USER}/gfarm2fs"
#
# # "chown -R" is too slow.
# COPY . /tmp/gfarm
# COPY gfarm2fs /tmp/gfarm2fs
# RUN rsync -a --chown=${GFDOCKER_PRIMARY_USER}:${GFDOCKER_PRIMARY_USER} \
# 	/tmp/gfarm "/home/${GFDOCKER_PRIMARY_USER}"
# RUN rsync -a --chown=${GFDOCKER_PRIMARY_USER}:${GFDOCKER_PRIMARY_USER} \
# 	/tmp/gfarm2fs "/home/${GFDOCKER_PRIMARY_USER}"
#
# RUN "/home/${GFDOCKER_PRIMARY_USER}/gfarm/docker/dev/common/setup-univ.sh"

set -eux

: $GFDOCKER_USERNAME_PREFIX
: $GFDOCKER_PRIMARY_USER
: $GFDOCKER_NUM_GFMDS
: $GFDOCKER_NUM_GFSDS
: $GFDOCKER_NUM_USERS
: $GFDOCKER_HOSTNAME_PREFIX_GFMD
: $GFDOCKER_HOSTNAME_PREFIX_GFSD

: $GFDOCKER_DIST_NAME

gfarm_src_path="/home/${GFDOCKER_PRIMARY_USER}/gfarm"

for u in _gfarmmd _gfarmfs; do
  useradd -m -s /bin/bash "$u"
done

ca_key_pass=pw

grid-ca-create -pass "$ca_key_pass" -noint \
  -subject 'cn=GlobusSimpleCA,ou=GlobusTest,o=Grid'
ls globus_simple_ca_*.tar.gz \
  | sed -E 's/^globus_simple_ca_(.*)\.tar\.gz$/\1/' > /ca_hash

# opensuse only? difference of globus versions?
if [ "$GFDOCKER_DIST_NAME" = opensuse ]; then
  cd /etc/grid-security/
  mv hostcert.pem myca-hostcert.pem
  mv hostcert_request.pem myca-hostcert_request.pem
  mv hostkey.pem myca-hostkey.pem
fi

force_yes=y

for i in $(seq 1 "$GFDOCKER_NUM_GFMDS"); do
  fqdn="${GFDOCKER_HOSTNAME_PREFIX_GFMD}${i}"
  echo "$force_yes" \
    | grid-cert-request -verbose -nopw -prefix "$fqdn" -host "$fqdn" \
      -ca "$(cat /ca_hash)"
  echo "$ca_key_pass" \
    | grid-ca-sign -in "/etc/grid-security/${fqdn}cert_request.pem" \
      -out "/etc/grid-security/${fqdn}cert.pem"
done

GRID_MAPFILE=/etc/grid-security/grid-mapfile

for i in $(seq 1 "$GFDOCKER_NUM_GFSDS"); do
  fqdn="${GFDOCKER_HOSTNAME_PREFIX_GFSD}${i}"
  common_name="gfsd/${fqdn}"
  cert_path="/etc/grid-security/gfsd-${fqdn}"
  echo "$force_yes" \
    | grid-cert-request -verbose -nopw -prefix gfsd -host "$fqdn" \
      -dir "$cert_path" -commonname "$common_name" -service gfsd \
      -ca "$(cat /ca_hash)"
  echo "$ca_key_pass" \
    | grid-ca-sign -in "${cert_path}/gfsdcert_request.pem" \
      -out "${cert_path}/gfsdcert.pem"
  chown -R _gfarmfs "$cert_path"
  echo "/O=Grid/OU=GlobusTest/CN=${common_name} @host@ ${fqdn}" \
    >> "$GRID_MAPFILE"
done

base_ssh_config="${gfarm_src_path}/docker/dev/common/ssh_config"
echo >> /etc/sudoers
echo '# for Gfarm' >> /etc/sudoers
for i in $(seq 1 "$GFDOCKER_NUM_USERS"); do
  user="${GFDOCKER_USERNAME_PREFIX}${i}"
  # User account is made by caller of this script.
  # see "Usage (Dockerfile)" for details.
  echo "${user} ALL=(root, _gfarmfs, _gfarmmd) NOPASSWD: /usr/bin/gfservice-agent" \
    >> /etc/sudoers
  echo "${user} ALL=(root, _gfarmfs, _gfarmmd) NOPASSWD: /usr/local/bin/gfservice-agent" \
    >> /etc/sudoers
  echo "${user} ALL=NOPASSWD: ALL" >> /etc/sudoers
  ssh_dir="/home/${user}/.ssh"
  mkdir -m 0700 -p "$ssh_dir"
  ssh-keygen -f "${ssh_dir}/key-gfarm" -N ''
  authkeys="${ssh_dir}/authorized_keys"
  cp "${ssh_dir}/key-gfarm.pub" "$authkeys"
  chmod 0644 "$authkeys"
  ssh_config="${ssh_dir}/config"
  cp "$base_ssh_config" "$ssh_config"
  chmod 0644 "$ssh_config"
  mkdir -m 0700 -p "${ssh_dir}/ControlMasters"
  chown -R "${user}:${user}" "$ssh_dir"
  globus_dir="/home/${user}/.globus/"
  su - "$user" -c ' \
    grid-cert-request -verbose -cn "$(whoami)" -nopw -ca "$(cat /ca_hash)" \
  '
  echo "$ca_key_pass" \
    | grid-ca-sign -in "${globus_dir}/usercert_request.pem" \
      -out "${globus_dir}/usercert.pem"
  echo "/O=Grid/OU=GlobusTest/OU=GlobusSimpleCA/CN=${user} ${user}" \
    >> "$GRID_MAPFILE"
done

base_gfservicerc="${gfarm_src_path}/docker/dev/common/rc.gfservice"
base_gfarm2rc="${gfarm_src_path}/docker/dev/common/rc.gfarm2rc"
for i in $(seq 1 "$GFDOCKER_NUM_USERS"); do
  user="${GFDOCKER_USERNAME_PREFIX}${i}"
  gfservicerc="/home/${user}/.gfservice"
  cp "$base_gfservicerc" "$gfservicerc"
  chmod 0644 "$gfservicerc"
  chown "${user}:${user}" "$gfservicerc"
  gfarm2rc="/home/${user}/.gfarm2rc"
  cp "$base_gfarm2rc" "$gfarm2rc"
  chmod 0644 "$gfarm2rc"
  chown "${user}:${user}" "$gfarm2rc"
done

su - "$GFDOCKER_PRIMARY_USER" -c " \
  cd ~/gfarm \
    && for f in ~/gfarm/docker/dev/patch/*.patch; do \
      patch -p0 < \${f}; \
    done \
"
