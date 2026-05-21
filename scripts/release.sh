#!/usr/bin/env bash
set -e

current=$(cat VERSION)

IFS='.' read -r major minor patch <<< "$current"

echo "Current version: $current"

echo "Select bump type:"
echo "1) patch"
echo "2) minor"
echo "3) major"
read -r choice

case "$choice" in
  1)
    patch=$((patch + 1))
    ;;
  2)
    minor=$((minor + 1))
    patch=0
    ;;
  3)
    major=$((major + 1))
    minor=0
    patch=0
    ;;
  *)
    echo "Invalid choice"
    exit 1
    ;;
esac

new_version="$major.$minor.$patch"

echo "$new_version" > VERSION
echo "Updated VERSION -> $new_version"

git add VERSION
git commit -m "bump version to $new_version"
git tag "v$new_version"