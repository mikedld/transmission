set(WEB_SOURCES
    src/about-dialog.js
    src/action-manager.js
    src/alert-dialog.js
    src/context-menu.js
    src/file-row.js
    src/formatter.js
    src/inspector.js
    src/labels-dialog.js
    src/main.js
    src/move-dialog.js
    src/notifications.js
    src/open-dialog.js
    src/overflow-menu.js
    src/prefs-dialog.js
    src/prefs.js
    src/remote.js
    src/remove-dialog.js
    src/rename-dialog.js
    src/shortcuts-dialog.js
    src/statistics-dialog.js
    src/torrent.js
    src/torrent-row.js
    src/transmission.js
    src/utils.js)

set(WEB_STYLES
    assets/css/transmission-app.scss)

set(WEB_IMAGES
    assets/img/box.svg
    assets/img/chevron-down.svg
    assets/img/chevron-up.svg
    assets/img/file-text.svg
    assets/img/film.svg
    assets/img/folder.svg
    assets/img/horizontal-rule.svg
    assets/img/image.svg
    assets/img/lock-fill.svg
    assets/img/logo.png
    assets/img/magnet.svg
    assets/img/music.svg
    assets/img/package.svg
    assets/img/README.md
    assets/img/router.svg
    assets/img/type.svg
    assets/img/turtle.svg)

add_custom_command(
    OUTPUT
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css.LEGAL.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css.map"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js.LEGAL.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js.map"
    WORKING_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/esbuild.mjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/prettier.config.cjs"
        "${CMAKE_CURRENT_SOURCE_DIR}/stylelint.config.cjs"
        "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets" "assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/public_html" "public_html"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "src"
    COMMAND "${NPM}" ci --no-audit --no-fund --no-progress
    COMMAND "${NPM}" run build
    DEPENDS
        ${WEB_SOURCES}
        ${WEB_STYLES}
        ${WEB_IMAGES})

add_custom_target("${TR_NAME}-web"
    ALL
    DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css.LEGAL.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.css.map"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js.LEGAL.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/public_html/transmission-app.js.map"
    SOURCES
        ${WEB_SOURCES})

set(TR_WEB_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/public_html PARENT_SCOPE)
