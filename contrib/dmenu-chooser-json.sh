#!/usr/bin/env bash

chooser=$1
jq  '.monitor | .[] | .name + ": " + .make + " " + .model' | tr -d '"' | ${chooser:="wofi -d"} | cut -d : -f 1
