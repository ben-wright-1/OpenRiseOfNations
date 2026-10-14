#!/bin/bash
# Package OpenRiseOfNations as a macOS app and a .dmg disk image:
#   build/OpenRiseOfNations.app            (universal: Apple Silicon + Intel, ad-hoc signed)
#   build/OpenRiseOfNations-<version>.dmg  (the app plus a shortcut to /Applications)
# Needs only Apple's Command Line Tools. Set SKIP_BUILD=1 to reuse an existing build/ronmap.
# Optional real signing / notarisation: set CODESIGN_ID="Developer ID Application: ..." (and notarise the .dmg yourself).
set -euo pipefail
cd "$(dirname "$0")"

NAME=OpenRiseOfNations
VERSION=$(cat VERSION 2>/dev/null || echo 0.0.0)
APP=build/$NAME.app
DMG=build/$NAME-$VERSION.dmg

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
	UNIVERSAL=1 bash build-mac.sh
fi

echo "Assembling $APP ..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build/ronmap "$APP/Contents/MacOS/$NAME"

# Icon: tools/AppIcon.png (1024 px, macOS icon grid) scaled into an .iconset.
rm -rf build/AppIcon.iconset
mkdir -p build/AppIcon.iconset
for s in 16 32 128 256 512; do
	sips -z $s $s tools/AppIcon.png --out "build/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
	sips -z $((s * 2)) $((s * 2)) tools/AppIcon.png --out "build/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns build/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"

cp LICENSE README.md "$APP/Contents/Resources/"
# The front end's own files (the main-menu logo); everything else is read from the player's install.
cp apps/map-mac/resources/* "$APP/Contents/Resources/"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleDisplayName</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>org.openriseofnations.app</string>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSHumanReadableCopyright</key><string>GPL-3.0. Not affiliated with Microsoft or Big Huge Games.</string>
</dict>
</plist>
EOF

echo "Signing ..."
codesign --force --deep --options runtime --sign "${CODESIGN_ID:--}" "$APP"
codesign --verify --verbose=1 "$APP"

echo "Creating $DMG ..."
STAGE=build/dmg-stage
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp README.md "$STAGE/README.md"
hdiutil create -volname "$NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null 2>&1
rm -rf "$STAGE"

echo
echo "Done:"
echo "  $APP   ($(lipo -archs "$APP/Contents/MacOS/$NAME"))"
echo "  $DMG   ($(du -h "$DMG" | cut -f1))"
