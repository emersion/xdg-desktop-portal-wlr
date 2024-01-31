#!/usr/bin/env bash

chooser=$1

read -r json
echo "$json" >&2
output=$(echo $json | jq  '.monitors | .[] | (.id|tostring) + ":" + .name + ":" + .make + ":" + .model' | tr -d '"' | ${chooser:="wofi -d"})
ret=$!
version=$(echo $json | jq '.version|tostring')
revision=$(echo $json | jq '.revision|tostring')
target_type=monitor
IFS=":" read -r id name make model <<<"$output"
echo "version:$version" >&2
echo "revision:$revision" >&2
echo "target_type:$target_type" >&2
echo "name:$name" >&2
echo "model:$model" >&2
echo "make:$make" >&2
echo "id $id" >&2

jq -c --null-input \
  --argjson version $version \
  --argjson revision $revision \
  --arg target_type "$target_type" \
  --arg name "$name" \
  --arg model "$model" \
  --arg make "$make" \
  --argjson id $id \
  '{"version": $version, "revision": $revision, "target_type": $target_type, "target": { "name": $name, "model": $model, "make": $make, "id": $id }}'
exit $ret
