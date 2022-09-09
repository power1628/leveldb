#!/bin/bash

docker run --cap-add=SYS_PTRACE --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
           --restart=always \
           -v /var/run/docker.sock:/var/run/docker.sock \
           -v $HOME:$HOME \
           --name lmdbbench -dit \
           lmdb:latest \
           bash

#           -p 2002:22 \

