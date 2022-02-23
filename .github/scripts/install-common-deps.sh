#!/bin/sh -eux

case "$(uname -s)" in
        CYGWIN*|MINGW32*|MSYS*|MINGW*)
                SUDO=
                ;;

        *)
                SUDO="sudo "
                ;;
esac

# only download here, compilation is handled per-platform
download_cineform() {(
        cd $GITHUB_WORKSPACE
        git clone --depth 1 https://github.com/gopro/cineform-sdk
        mkdir cineform-sdk/build
)}

install_ews() {
        ${SUDO}curl -LS https://raw.githubusercontent.com/hellerf/EmbeddableWebServer/master/EmbeddableWebServer.h -o /usr/local/include/EmbeddableWebServer.h
}

install_juice() {
(
        git clone https://github.com/paullouisageneau/libjuice.git
        mkdir libjuice/build
        cd libjuice/build
        cmake -DCMAKE_INSTALL_PREFIX=/usr/local -G "Unix Makefiles" ..
        make -j $(nproc)
        ${SUDO}make install
)
}

install_pcp() {
        git clone https://github.com/libpcp/pcp.git
        (
                cd pcp
                ./autogen.sh || true # autogen exits with 1
                CFLAGS=-fPIC ./configure --disable-shared
                make -j 5
                ${SUDO}make install
        )
        rm -rf pcp
}

install_zfec() {(
        git clone --depth 1 https://github.com/tahoe-lafs/zfec zfec
        ${SUDO}mkdir -p /usr/local/src
        ${SUDO}mv zfec/zfec /usr/local/src
)}

download_cineform
install_ews
install_juice
install_pcp
install_zfec

