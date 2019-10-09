// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const SourceFile = `
{{- define "GenerateSourceFile" -}}
// WARNING: This file is machine generated by fidlgen.

#![allow(
	deprecated, // FIDL Impl struct pattern is referenced internally
	unused_parens, // one-element-tuple-case is not a tuple
	unused_mut, // not all args require mutation, but many do
	nonstandard_style, // auto-caps does its best, but is not always successful
)]

#[cfg(target_os = "fuchsia")]
#[allow(unused_imports)]
use fuchsia_zircon as zx;
#[allow(unused_imports)]
use fuchsia_zircon_status as zx_status;
#[allow(unused_imports)]
use fidl::{
	fidl_bits,
	fidl_enum,
	fidl_empty_struct,
	fidl_struct,
	fidl_table,
	fidl_union,
	fidl_xunion,
};

{{ range $bits := .Bits -}}
{{ template "BitsDeclaration" $bits }}
{{ end -}}
{{ range $const := .Consts -}}
{{ template "ConstDeclaration" $const }}
{{ end -}}
{{ range $enum := .Enums -}}
{{ template "EnumDeclaration" $enum }}
{{ end -}}
{{ range $result := .Results -}}
{{ template "ResultDeclaration" $result}}
{{ end -}}
{{ range $union := .Unions -}}
{{ template "UnionDeclaration" $union }}
{{ end -}}
{{ range $xunion := .XUnions -}}
{{ template "XUnionDeclaration" $xunion }}
{{ end -}}
{{ range $struct := .Structs -}}
{{ template "StructDeclaration" $struct }}
{{ end -}}
{{ range $table := .Tables -}}
{{ template "TableDeclaration" $table }}
{{ end -}}
{{ range $interface := .Interfaces -}}
{{ range $transport, $_ := .Transports -}}
{{ if eq $transport "Channel" -}}{{ template "InterfaceDeclaration" $interface }}{{- end }}
{{ end -}}
{{ end -}}
{{- end -}}
`
