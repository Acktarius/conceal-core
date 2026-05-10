#!/bin/bash

# Conceal Migration Tool Launcher
# All rights reserved (c) 2026 Conceal Network & Conceal Dev
# run with: ./migrate-gui.sh
# GUI version for Zenity (needs zenity installed: sudo apt install zenity)
# else run with: ./migrate.sh

set -euo pipefail

APP_TITLE="Conceal MDBX Migration Tool"
START_DAEMON=false
DAEMON_FLAGS=""
SIZE_FLAGS=""
EXTRA_FLAGS=""

#Couleurs
case "$TERM" in
        xterm-256color)
        WHITE=$(tput setaf 7 bold)
        ORANGE=$(tput setaf 202)
        GRIS=$(tput setaf 245)
		LINK=$(tput setaf 4 smul)
        TURNOFF=$(tput sgr0)
        ;;
        *)
        WHITE=''
		ORANGE=''
        GRIS=''
		LINK=''
        TURNOFF=''
        ;;
esac

#Presentation
presentation (){
clear    
echo -e "${GRIS}####################################################################"
echo -e "#                                                                  #"
echo -e "#-->${ORANGE}              ${APP_TITLE}        ${TURNOFF}${GRIS}           <--#"
echo -e "#                                                                  #"
echo -e "#                                                                  #"
echo -e "#                                                                  #"
echo -e "####################################################    ${WHITE}.::::."
echo -e "${GRIS}#                                                   ${WHITE}.:---=--=--::."
echo -e "${GRIS}#  You will need to select your old directory    ${WHITE}   -=:+-.  .-=:=:"
echo -e "${GRIS}#  (${WHITE}Ctrl${GRIS} + ${WHITE}H${GRIS} to show hidden files)               ${WHITE}   -=:+."
echo -e "${GRIS}#  then select new directory for mdbx database.     ${WHITE}-=:+."
echo -e "${GRIS}#                                                   ${WHITE}-=:+."
echo -e "${GRIS}#  And choose your options.     		    ${WHITE}-=:=."
echo -e "${GRIS}#                                                   ${WHITE}-+:-:    .::."
echo -e "${GRIS}#						    ${WHITE}-+==------===-"
echo -e "${GRIS}####################################################   ${WHITE}:-=-==-:${TURNOFF}\n\n"

}


require_binaries() {
    if [ ! -f "./conceald-migrate" ]; then
        zenity --error \
            --title="$APP_TITLE" \
            --text="conceald-migrate not found in current directory.\n\nMake sure you're in the build/src directory."
        exit 1
    fi

    if [ ! -f "../conceald" ]; then
        zenity --error \
            --title="$APP_TITLE" \
            --text="conceald not found in current directory.\n\nMake sure you're in the build/src directory."
        exit 1
    fi
}

# Start conceald in a new gnome-terminal window so this script can finish and logs stay visible.
launch_conceald_terminal() {
    local new_dir=$1
    local conceald_abs
    conceald_abs="$(cd .. && pwd)/conceald"

    if command -v gnome-terminal >/dev/null 2>&1; then
        export _MIGRATE_GUI_CONCEALD="$conceald_abs"
        export _MIGRATE_GUI_DATADIR="$new_dir"
        export _MIGRATE_GUI_EXTRA="${DAEMON_FLAGS:-}"
        gnome-terminal \
            --title="$APP_TITLE - conceald" \
            -- bash -c '
                "$_MIGRATE_GUI_CONCEALD" --use-mdbx --data-dir "$_MIGRATE_GUI_DATADIR" $_MIGRATE_GUI_EXTRA
                echo
                echo "conceald exited with code $?"
                exec bash
            '
        unset _MIGRATE_GUI_CONCEALD _MIGRATE_GUI_DATADIR _MIGRATE_GUI_EXTRA
    else
        zenity --warning \
            --title="$APP_TITLE" \
            --text="gnome-terminal not found. Starting conceald in the background (output not shown)."
        "$conceald_abs" --use-mdbx --data-dir "$new_dir" ${DAEMON_FLAGS:-} &
    fi
}

main() {
    local OLD_DIR NEW_DIR SIZE_MODE START_MODE SIZE_GB SUMMARY MIGRATE_EXIT=0

    presentation

    if ! command -v zenity >/dev/null 2>&1; then
        echo -e "${GRIS}zenity is not installed.${TURNOFF}"
        echo -e "${GRIS}I will install zenity:${TURNOFF}"
        echo -e "${WHITE}  sudo apt install zenity${TURNOFF}"
        echo -e "${GRIS}and rerun this script.${TURNOFF} \n"
        echo -e "${GRIS}Or I will use the CLI script:${TURNOFF}"
        echo -e "${WHITE}  ./migrate.sh${TURNOFF}"
        exit 1
    fi

    require_binaries

    OLD_DIR=$(
        zenity --file-selection \
            --title="Select OLD blockchain data directory" \
            --directory \
            --filename="$HOME/"
    ) || exit 0

    if [ -z "$OLD_DIR" ]; then
        zenity --warning \
            --title="$APP_TITLE" \
            --text="Old directory is required."
        exit 0
    fi

    if [ ! -d "$OLD_DIR" ]; then
        zenity --warning \
            --title="$APP_TITLE" \
            --text="Directory does not exist:\n$OLD_DIR"
        exit 0
    fi

    if [ ! -f "$OLD_DIR/blocks.dat" ] || [ ! -f "$OLD_DIR/blockindexes.dat" ]; then
        zenity --warning \
            --title="$APP_TITLE" \
            --text="Required file missing.\n\nThe selected old directory must contain:\n- blocks.dat\n- blockindexes.dat"
        exit 0
    fi

    NEW_DIR=$(
        zenity --file-selection \
            --title="Select NEW MDBX database directory" \
            --directory \
            --filename="$HOME/"
    ) || exit 0

    if [ -z "$NEW_DIR" ]; then
        zenity --warning \
            --title="$APP_TITLE" \
            --text="New directory is required."
        exit 0
    fi

    if [ "$OLD_DIR" = "$NEW_DIR" ]; then
        zenity --warning \
            --title="$APP_TITLE" \
            --text="Old and new directories must be different."
        exit 0
    fi

    SIZE_MODE=$(
        zenity --list \
            --title="Migration mode" \
            --text="Choose the database size scenario:" \
            --radiolist \
            --column="Pick" \
            --column="Option" \
            --column="Description" \
            TRUE "default" "Default size limit (128 GB)" \
            FALSE "custom" "Custom size limit in GB" \
            FALSE "nolimit" "No limit" \
            FALSE "testnet" "Testnet mode" \
            --height=320 \
            --width=720
    ) || exit 0

    case "$SIZE_MODE" in
        default)
            SIZE_FLAGS=""
            ;;
        custom)
            SIZE_GB=$(
                zenity --entry \
                    --title="Custom size limit" \
                    --text="Enter size limit in GB:" \
                    --entry-text="128"
            ) || exit 0

            if [ -n "$SIZE_GB" ]; then
                if [[ "$SIZE_GB" =~ ^[0-9]+$ ]] && [ "$SIZE_GB" -gt 0 ]; then
                    SIZE_FLAGS="--size-limit $SIZE_GB"
                else
                    zenity --warning \
                        --title="$APP_TITLE" \
                        --text="Size must be a positive integer in GB."
                    exit 0
                fi
            else
                SIZE_FLAGS=""
            fi
            ;;
        nolimit)
            SIZE_FLAGS="--no-limit"
            ;;
        testnet)
            SIZE_FLAGS="--testnet"
            ;;
        *)
            SIZE_FLAGS=""
            ;;
    esac

    EXTRA_FLAGS=""
    if zenity --question \
        --title="$APP_TITLE" \
        --text="Do you want to migrate in safe mode?"; then
        EXTRA_FLAGS="--safe-mode"
    fi

    START_MODE=$(
        zenity --list \
            --title="Daemon startup" \
            --text="What should happen after migration?" \
            --radiolist \
            --column="Pick" \
            --column="Option" \
            --column="Description" \
            TRUE "start" "Start daemon immediately" \
            FALSE "custom" "Start daemon with custom flags" \
            FALSE "nostart" "Do not start daemon" \
            --height=280 \
            --width=720
    ) || exit 0

    case "$START_MODE" in
        start)
            START_DAEMON=true
            DAEMON_FLAGS=""
            ;;
        custom)
            START_DAEMON=true
            DAEMON_FLAGS=$(
                zenity --entry \
                    --title="Daemon flags" \
                    --text="Enter additional daemon flags:" \
                    --entry-text="--log-level 3 --no-console"
            ) || exit 0
            ;;
        nostart)
            START_DAEMON=false
            DAEMON_FLAGS=""
            ;;
        *)
            START_DAEMON=false
            DAEMON_FLAGS=""
            ;;
    esac

    SUMMARY="Old directory: $OLD_DIR
New directory: $NEW_DIR
Migration flags: ${SIZE_FLAGS:-default (128 GB)}
Extra flags: ${EXTRA_FLAGS:-none}
Auto-start daemon: $START_DAEMON"

    if [ "$START_DAEMON" = true ] && [ -n "$DAEMON_FLAGS" ]; then
        SUMMARY="$SUMMARY
Daemon flags: $DAEMON_FLAGS"
    fi

    zenity --question \
        --title="$APP_TITLE" \
        --width=720 \
        --ok-label="Start migration" \
        --cancel-label="Cancel" \
        --text="$SUMMARY" || exit 0

    echo -e "\n ${WHITE}--- conceald-migrate (live output) ---${TURNOFF}\n"

    set +e
    ./conceald-migrate --old-dir "$OLD_DIR" --new-dir "$NEW_DIR" ${SIZE_FLAGS:-} ${EXTRA_FLAGS:-}
    MIGRATE_EXIT=$?
    set -e

    echo ""
    if [ "$MIGRATE_EXIT" -eq 0 ]; then
        echo -e "${WHITE}--- migration finished successfully ---${TURNOFF}"
    else
        echo -e "${ORANGE}--- migration failed or interrupted (exit $MIGRATE_EXIT) ---${TURNOFF}"
    fi
    echo ""

    if [ "$MIGRATE_EXIT" -eq 0 ]; then
        if [ "$START_DAEMON" = true ]; then
            zenity --info \
                --title="$APP_TITLE" \
                --text="Migration completed successfully.\n\nThe daemon will start in a new terminal window."
            launch_conceald_terminal "$NEW_DIR"
        else
            zenity --info \
                --title="$APP_TITLE" \
                --text="Migration completed successfully.\n\nTo start later, run:\n./conceald --use-mdbx --data-dir \"$NEW_DIR\""
        fi
    else
        zenity --warning \
            --title="$APP_TITLE" \
            --text="Migration failed or was interrupted.\n\nFix the issue and run the migration again before starting the daemon.\n\n(Advanced: you may still try manually:\n./conceald --use-mdbx --data-dir \"$NEW_DIR\")"
    fi
}

main "$@"