import pathlib
import argparse
import os
import yaml
from typing import List, Dict, Union, Tuple, Sequence, Optional
from tools.codegen.gen import FileManager, get_grouped_native_functions, parse_native_yaml
from tools.codegen.model import (BackendIndex, BackendMetadata, DispatchKey,
                                 NativeFunction, NativeFunctionsGroup, OperatorName)
from tools.codegen.selective_build.selector import SelectiveBuilder
from tools.codegen.utils import Target, concatMap
import tools.codegen.dest as dest
import tools.codegen.api.dispatcher as dispatcher

try:
    # use faster C loader if available
    from yaml import CSafeLoader as Loader
except ImportError:
    from yaml import SafeLoader as Loader  # type: ignore


# Parses the external backend's yaml, and adds a new BackendIndex for the backend's dispatch key.
# Returns a Tuple of (backend_key, autograd_key, cpp_namespace, updated BackendIndex mapping)
def parse_backend_yaml(
        backend_yaml_path: str,
        grouped_native_functions: Sequence[Union[NativeFunction, NativeFunctionsGroup]],
        backend_indices: Dict[DispatchKey, BackendIndex]
) -> Tuple[Optional[DispatchKey], Optional[DispatchKey], str, Dict[DispatchKey, BackendIndex]]:

    native_functions_map: Dict[OperatorName, NativeFunction] = {
        f.func.name: f
        for f in concatMap(lambda f: [f] if isinstance(f, NativeFunction) else list(f.functions()), grouped_native_functions)
    }

    with open(backend_yaml_path, 'r') as f:
        yaml_values = yaml.load(f, Loader=Loader)
    assert isinstance(yaml_values, dict)

    valid_keys = ['backend', 'cpp_namespace', 'supported', 'autograd']

    backend = yaml_values.pop('backend', None)
    assert backend is not None, 'You must provide a value for "backend"'
    cpp_namespace = yaml_values.pop('cpp_namespace', None)
    assert cpp_namespace is not None, 'You must provide a value for "cpp_namespace"'

    supported = yaml_values.pop('supported', [])
    if supported is None:
        supported = []  # Allow an empty list of supported ops
    assert isinstance(supported, list), f'expected "supported" to be a list, but got: {supported} (of type {type(supported)})'
    supported_autograd = yaml_values.pop('autograd', [])
    assert isinstance(supported, list), f'expected "autograd" to be a list, but got: {supported_autograd}'

    assert len(yaml_values.keys()) == 0, \
        f'{backend_yaml_path} contains unexpected keys: {", ".join(yaml_values.keys())}. \
Only the following keys are supported: {", ".join(valid_keys)}'

    def add_backend_index(backend_ops: List[str], backend: str, *, is_autograd: bool = False) -> DispatchKey:
        if is_autograd:
            # TODO: add better error messages here (later PR)
            backend_key = DispatchKey.parse(f'Autograd{backend}')
        else:
            backend_key = DispatchKey.parse(backend)
        metadata: Dict[OperatorName, BackendMetadata] = {}
        for op in backend_ops:
            op_name = OperatorName.parse(op)
            assert op_name in native_functions_map, f"Found an invalid operator name: {op_name}"
            # See Note [External Backends Follow Dispatcher API]
            kernel_name = dispatcher.name(native_functions_map[op_name].func)
            # TODO: allow structured external backends later.
            m = BackendMetadata(kernel=kernel_name, structured=False, external=True)
            metadata[op_name] = m
        assert backend_key not in backend_indices
        # TODO: currently hardcoding the fact that XLA implements out/inplace in terms of functional ops,
        # this should eventually be toggleable per-backend.
        backend_indices[backend_key] = BackendIndex(dispatch_key=backend_key, use_out_as_primary=False, index=metadata)
        return backend_key

    backend_key: Optional[DispatchKey] = None
    if len(supported) > 0:
        backend_key = add_backend_index(supported, backend)

    autograd_key: Optional[DispatchKey] = None
    if len(supported_autograd) > 0:
        autograd_key = add_backend_index(supported_autograd, backend, is_autograd=True)

    return backend_key, autograd_key, cpp_namespace, backend_indices

def main() -> None:
    parser = argparse.ArgumentParser(description='Generate backend stub files')
    parser.add_argument(
        '-s',
        '--source_yaml',
        help='path to source yaml file containing operator external definitions')
    parser.add_argument(
        '-o', '--output_dir', help='output directory')
    parser.add_argument(
        '--dry_run', type=bool, default=False, help='output directory')
    options = parser.parse_args()

    run(options.source_yaml, options.output_dir, options.dry_run)

def run(source_yaml: str, output_dir: str, dry_run: bool) -> None:

    # Assumes that this file lives at PYTORCH_ROOT/tools/codegen/gen_backend_stubs.py
    pytorch_root = pathlib.Path(__file__).parent.parent.parent.absolute()
    template_dir = os.path.join(pytorch_root, "aten/src/ATen/templates")

    def make_file_manager(install_dir: str) -> FileManager:
        return FileManager(install_dir=install_dir, template_dir=template_dir, dry_run=dry_run)

    fm = make_file_manager(output_dir)

    native_yaml_path = os.path.join(pytorch_root, 'aten/src/ATen/native/native_functions.yaml')
    native_functions, backend_indices = parse_native_yaml(native_yaml_path)
    grouped_native_functions = get_grouped_native_functions(native_functions, backend_indices)
    backend_key, autograd_key, cpp_namespace, backend_indices = parse_backend_yaml(
        source_yaml, grouped_native_functions, backend_indices)

    selector = SelectiveBuilder.get_nop_selector()


    # TODO: handle cases when yaml contains zero ops properly in a later PR.
    if backend_key is not None and autograd_key is not None:
        backend_dispatch_key: DispatchKey = backend_key
        autograd_dispatch_key: DispatchKey = autograd_key
        generated_comment = 'Autogenerated file by gen_backend_stubs.py. Do not edit directly!'
        fm.write('aten_xla_type.h', lambda: {
            'generated_comment': generated_comment,
            'cpp_namespace': cpp_namespace,
            'dispatch_xla_declarations': list(concatMap(
                # Convert to a set first to remove duplicate kernel names.
                # Backends are allowed to repeat kernel names; only generate the declaration once!
                lambda f: list(set(concatMap(
                    lambda dispatch_key: dest.compute_native_function_declaration(f, backend_indices[dispatch_key]),
                    [backend_dispatch_key, autograd_dispatch_key]))),
                grouped_native_functions)),
        })

        fm.write('aten_xla_type_default.h', lambda: {
            'generated_comment': generated_comment,
            'cpp_namespace': cpp_namespace,
            'dispatch_aten_fallback_declarations': list(
                # Using a set to dedup: we end up with duplicate definitions,
                # because ops that have neither an XLA nor an AutogradXLA kernel
                # will get a CPU fallback from both calls.
                set(concatMap(
                    dest.GenExternalAtenFallback(Target.NAMESPACED_DECLARATION, backend_indices[backend_dispatch_key]),
                    grouped_native_functions
                )) | set(concatMap(
                    dest.GenExternalAtenFallback(Target.NAMESPACED_DECLARATION, backend_indices[autograd_dispatch_key]),
                    grouped_native_functions
                ))
            ),
        })

        fm.write('aten_xla_type_default.cpp', lambda: {
            'generated_comment': generated_comment,
            'cpp_namespace': cpp_namespace,
            # TODO: after cpu fallbacks are moved to a boxed kernel,
            # merge registrations / definitions into RegisterDispatchKey
            'dispatch_aten_fallback_definitions': list(
                set(concatMap(
                    dest.GenExternalAtenFallback(Target.NAMESPACED_DEFINITION, backend_indices[backend_dispatch_key]),
                    grouped_native_functions
                )) | set(concatMap(
                    dest.GenExternalAtenFallback(Target.NAMESPACED_DEFINITION, backend_indices[autograd_dispatch_key]),
                    grouped_native_functions
                ))
            ),
            'dispatch_registrations': list(concatMap(
                dest.GenExternalAtenFallback(Target.REGISTRATION, backend_indices[backend_dispatch_key]),
                grouped_native_functions
            )),
            'dispatch_autograd_registrations': list(concatMap(
                dest.GenExternalAtenFallback(Target.REGISTRATION, backend_indices[autograd_dispatch_key]),
                grouped_native_functions
            )),
        })

if __name__ == '__main__':
    main()
