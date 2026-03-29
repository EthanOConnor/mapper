#!/bin/sh

set -eu

prepend_path() {
	name=$1
	value=$2

	if [ ! -d "$value" ]; then
		return
	fi

	eval "current=\${$name-}"
	if [ -n "$current" ]; then
		export "$name=$value:$current"
	else
		export "$name=$value"
	fi
}

self_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=$(CDPATH= cd -- "$self_dir/.." && pwd)
package_name=openorienteering-mapper-coc
app_lib_dir="$prefix/lib/$package_name"
primary_plugin_dir="$app_lib_dir/plugins"
bundled_plugin_dir="$prefix/plugins"
data_dir="$prefix/share/$package_name"
doc_dir="$prefix/share/doc/$package_name"

prepend_path LD_LIBRARY_PATH "$app_lib_dir"
prepend_path LD_LIBRARY_PATH "$prefix/lib"
prepend_path QT_PLUGIN_PATH "$bundled_plugin_dir"
prepend_path QT_PLUGIN_PATH "$primary_plugin_dir"

if [ -d "$bundled_plugin_dir/platforms" ]; then
	export QT_QPA_PLATFORM_PLUGIN_PATH="$bundled_plugin_dir/platforms"
elif [ -d "$primary_plugin_dir/platforms" ]; then
	export QT_QPA_PLATFORM_PLUGIN_PATH="$primary_plugin_dir/platforms"
fi

if [ -d "$data_dir" ]; then
	export MAPPER_DATA_PATH="$data_dir"
fi

if [ -d "$doc_dir" ]; then
	export MAPPER_DOC_PATH="$doc_dir"
fi

if [ -d "$data_dir/proj" ]; then
	export PROJ_DATA="$data_dir/proj"
	export PROJ_LIB="$data_dir/proj"
fi

exec "$app_lib_dir/Mapper.bin" "$@"
