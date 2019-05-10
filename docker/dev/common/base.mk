# This file need config.mk

.PHONY: help ps build up down start stop shell shell-root shell-user \
		regress test-fo systest test-all valgrind-gfmd docker-compose \
		gen

ifndef BUILD_IMAGE_ORDER
$(error BUILD_IMAGE_ORDER is not defined)
endif

ifndef GFDOCKER_IP_VERSION
$(error GFDOCKER_IP_VERSION is not defined)
endif

ifndef GFDOCKER_SUBNET
$(error GFDOCKER_SUBNET is not defined)
endif

ifndef GFDOCKER_START_HOST_ADDR
$(error GFDOCKER_START_HOST_ADDR is not defined)
endif

ifneq ($(and $(GFDOCKER_PROXY_HOST),$(GFDOCKER_PROXY_PORT)),)
GFDOCKER_ENABLE_PROXY = true
endif

PROXY_URL = http://$(PROXY_HOST):$(PROXY_PORT)/
GFDOCKER_USERNAME_PREFIX = user
GFDOCKER_PRIMARY_USER = $(GFDOCKER_USERNAME_PREFIX)1
PRIMARY_CLIENT_CONTAINER = $(GFDOCKER_HOSTNAME_PREFIX_CLIENT)1
TOP = ../../../../..

DOCKER_BUILD_FLAGS = \
		--build-arg GFDOCKER_NUM_JOBS='$(GFDOCKER_NUM_JOBS)' \
		--build-arg GFDOCKER_USERNAME_PREFIX='$(GFDOCKER_USERNAME_PREFIX)' \
		--build-arg GFDOCKER_PRIMARY_USER='$(GFDOCKER_PRIMARY_USER)' \
		--build-arg GFDOCKER_NUM_GFMDS='$(GFDOCKER_NUM_GFMDS)' \
		--build-arg GFDOCKER_NUM_GFSDS='$(GFDOCKER_NUM_GFSDS)' \
		--build-arg GFDOCKER_NUM_USERS='$(GFDOCKER_NUM_USERS)' \
		--build-arg GFDOCKER_HOSTNAME_PREFIX_GFMD='$(GFDOCKER_HOSTNAME_PREFIX_GFMD)' \
		--build-arg GFDOCKER_HOSTNAME_PREFIX_GFSD='$(GFDOCKER_HOSTNAME_PREFIX_GFSD)'

ifdef GFDOCKER_ENABLE_PROXY
DOCKER_BUILD_FLAGS += \
		--build-arg http_proxy='$(PROXY_URL)' \
		--build-arg https_proxy='$(PROXY_URL)' \
		--build-arg GFDOCKER_PROXY_HOST='$(GFDOCKER_PROXY_HOST)' \
		--build-arg GFDOCKER_PROXY_PORT='$(GFDOCKER_PROXY_PORT)' \
		--build-arg GFDOCKER_ENABLE_PROXY='$(GFDOCKER_ENABLE_PROXY)'
endif

IMAGE_BASENAME = gfarm-dev

DOCKER = $(SUDO) docker
COMPOSE = $(SUDO) COMPOSE_PROJECT_NAME=gfarm-$(GFDOCKER_PRJ_NAME) \
	GFDOCKER_PRJ_NAME=$(GFDOCKER_PRJ_NAME) docker-compose
CONTSHELL_FLAGS = \
		--env GFDOCKER_PRIMARY_USER='$(GFDOCKER_PRIMARY_USER)' \
		--env GFDOCKER_NUM_GFMDS='$(GFDOCKER_NUM_GFMDS)' \
		--env GFDOCKER_NUM_GFSDS='$(GFDOCKER_NUM_GFSDS)' \
		--env GFDOCKER_NUM_CLIENTS='$(GFDOCKER_NUM_CLIENTS)' \
		--env GFDOCKER_HOSTNAME_PREFIX_GFMD='$(GFDOCKER_HOSTNAME_PREFIX_GFMD)' \
		--env GFDOCKER_HOSTNAME_PREFIX_GFSD='$(GFDOCKER_HOSTNAME_PREFIX_GFSD)' \
		--env GFDOCKER_HOSTNAME_PREFIX_CLIENT='$(GFDOCKER_HOSTNAME_PREFIX_CLIENT)'
CONTSHELL = $(COMPOSE) exec $(CONTSHELL_FLAGS) -u '$(GFDOCKER_PRIMARY_USER)' \
		'$(PRIMARY_CLIENT_CONTAINER)' bash

help:
	@echo 'Usage:'
	@echo '  make help'
	@echo '  make ps'
	@echo '  make build'
	@echo '  make down'
	@echo '  make prune'
	@echo '  make REMOVE_ALL_IMAGES'
	@echo '  make reborn'
	@echo '  make start'
	@echo '  make stop'
	@echo '  make shell'
	@echo '  make shell-user'
	@echo '  make shell-root'
	@echo '  make regress'
	@echo '  make test-fo'
	@echo '  make systest'
	@echo '  make systest-all'
	@echo '  ARGS="docker-compose args..." make docker-compose'
	@echo '  make test-all'
	@echo '  make valgrind-gfmd'
	@echo '  make centos7'
	@echo '  make opensuse'

define check_config
if ! [ -f $(TOP)/docker/dev/.shadow.config.mk ]; then \
	echo '.shadow.config.mk does not exist.' \
		'Containers are maybe down.' \
		'Please execute "make reborn".' 1>&2; \
	false; \
fi \
&& \
if ! diff -u $(TOP)/docker/dev/.shadow.config.mk \
		$(TOP)/docker/dev/config.mk 1>&2; then \
	echo 'Unexpected change of config.mk.' \
		'Change of config.mk can only be when container is down.' \
		1>&2; \
	false; \
fi
endef

ps:
	$(check_config)
	$(COMPOSE) ps

define build
for TAG in $(BUILD_IMAGE_ORDER); do \
	$(DOCKER) build -t "$(IMAGE_BASENAME):$${TAG}" \
		$(DOCKER_BUILD_FLAGS) \
		-f "$(TOP)/docker/dev/common/$${TAG}-Dockerfile" \
		'$(TOP)' || exit 1; \
done \
  && $(COMPOSE) build $(DOCKER_BUILD_FLAGS)
endef

build:
	$(build)

define down
$(COMPOSE) down && rm -f $(TOP)/docker/dev/.shadow.config.mk
endef

down:
	$(down)

define prune
$(DOCKER) system prune -f --volumes
endef

prune:
	$(prune)

REMOVE_ALL_IMAGES:
	$(DOCKER) system prune -a -f --volumes

define gen
TOP='$(TOP)' \
	GFDOCKER_PRIMARY_USER='$(GFDOCKER_PRIMARY_USER)' \
	GFDOCKER_NUM_GFMDS='$(GFDOCKER_NUM_GFMDS)' \
	GFDOCKER_NUM_GFSDS='$(GFDOCKER_NUM_GFSDS)' \
	GFDOCKER_NUM_CLIENTS='$(GFDOCKER_NUM_CLIENTS)' \
	GFDOCKER_IP_VERSION='$(GFDOCKER_IP_VERSION)' \
	GFDOCKER_SUBNET='$(GFDOCKER_SUBNET)' \
	GFDOCKER_START_HOST_ADDR='$(GFDOCKER_START_HOST_ADDR)' \
	GFDOCKER_HOSTNAME_PREFIX_GFMD='$(GFDOCKER_HOSTNAME_PREFIX_GFMD)' \
	GFDOCKER_HOSTNAME_PREFIX_GFSD='$(GFDOCKER_HOSTNAME_PREFIX_GFSD)' \
	GFDOCKER_HOSTNAME_PREFIX_CLIENT='$(GFDOCKER_HOSTNAME_PREFIX_CLIENT)' \
	GFDOCKER_AUTH_TYPE='$(GFDOCKER_AUTH_TYPE)' \
	'$(TOP)/docker/dev/common/gen.sh'
	cp $(TOP)/docker/dev/config.mk $(TOP)/docker/dev/.shadow.config.mk
endef

define up
$(COMPOSE) up -d \
  && $(CONTSHELL) -c '. ~/gfarm/docker/dev/common/up.rc'
endef

reborn:
	if [ -f $(TOP)/docker/dev/docker-compose.yml ]; then \
		$(down); \
	else \
		echo 'warn: docker-compose does not exist.' 1>&2; \
	fi
	$(gen)
	$(prune)
	$(build)
	$(up)

start:
	$(COMPOSE) start

stop:
	$(COMPOSE) stop

define shell_user
$(CONTSHELL)
endef

shell:
	$(check_config)
	$(shell_user)

shell-user:
	$(check_config)
	$(shell_user)

shell-root:
	$(check_config)
	$(COMPOSE) exec '$(PRIMARY_CLIENT_CONTAINER)' bash

define regress
$(CONTSHELL) -c '. ~/gfarm/docker/dev/common/regress.rc'
endef

regress:
	$(check_config)
	$(regress)

define test_fo
$(CONTSHELL) -c '. ~/gfarm/docker/dev/common/test-fo.rc'
endef

test-fo:
	$(check_config)
	$(test_fo)

define systest
$(CONTSHELL) -c '. ~/gfarm/docker/dev/common/systest.rc'
endef

systest:
	$(check_config)
	$(systest)

systest-all:
	$(check_config)
	@echo 'This target is unimplemented.' 1>&2
	@false

docker-compose:
	$(check_config)
	$(COMPOSE) $$ARGS

test-all:
	$(check_config)
	$(regress)
	$(test-fo)
	$(systest)

valgrind-gfmd:
	$(check_config)
	@echo 'This target is unimplemented.' 1>&2
	@false

centos7:
	$(DOCKER) run -it --rm 'centos:7' bash

opensuse:
	$(DOCKER) run -it --rm opensuse bash
