# Build environment for the CR-10 print server firmware.
# Official Espressif image: contains ESP-IDF, the Xtensa toolchain and Python deps.
ARG IDF_VERSION=v5.5.1
FROM espressif/idf:${IDF_VERSION}

# Mounted host folders are owned by another UID; let git (used by IDF for
# version strings and the component manager) operate on them.
RUN git config --system --add safe.directory '*'

# The image entrypoint exports the IDF environment for `docker run`, but VS Code
# dev-container terminals bypass it: export it for interactive shells too.
RUN echo 'source /opt/esp/idf/export.sh > /dev/null 2>&1' >> /root/.bashrc

# Component manager cache lives in a named volume (see idf.ps1 / idf.sh) so
# managed components are not re-downloaded on every build.
ENV IDF_COMPONENT_CACHE_PATH=/opt/idf-cache
ENV IDF_CCACHE_ENABLE=1
ENV CCACHE_DIR=/opt/idf-cache/ccache

WORKDIR /project
