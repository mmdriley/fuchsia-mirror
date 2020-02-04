// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:fxtest/fxtest.dart';

/// Deconstructed Fuchsia Package Url used to precisely target URL components.
class PackageUrl {
  final String host;
  final String packageName;
  final String packageVariant;
  final String hash;
  final String resourcePath;
  final String rawResource;
  PackageUrl({
    this.host,
    this.packageName,
    this.packageVariant,
    this.hash,
    this.resourcePath,
    this.rawResource,
  });

  PackageUrl.none()
      : host = null,
        hash = null,
        packageName = null,
        packageVariant = null,
        resourcePath = null,
        rawResource = null;

  /// Breaks out a canonical Fuchsia URL into its constituent parts.
  ///
  /// Parses something like
  /// `fuchsia-pkg://host/package_name/variant?hash=1234#PATH.cmx` into:
  ///
  /// ```dart
  /// PackageUrl(
  ///   'host': 'host',
  ///   'packageName': 'package_name',
  ///   'packageVariant': 'variant',
  ///   'hash': '1234',
  ///   'resourcePath': 'PATH.cmx',
  ///   'rawResource': 'PATH',
  /// );
  /// ```
  factory PackageUrl.fromString(String packageUrl) {
    Uri parsedUri = Uri.parse(packageUrl);

    if (parsedUri.scheme != 'fuchsia-pkg') {
      throw MalformedFuchsiaUrlException(packageUrl);
    }

    return PackageUrl(
      host: parsedUri.host,
      packageName:
          parsedUri.pathSegments.isNotEmpty ? parsedUri.pathSegments[0] : null,
      packageVariant:
          parsedUri.pathSegments.length > 1 ? parsedUri.pathSegments[1] : null,
      hash: parsedUri.queryParameters['hash'],
      resourcePath: parsedUri.fragment,
      rawResource: PackageUrl._removeExtension(parsedUri.fragment),
    );
  }

  static String _removeExtension(String resourcePath) {
    // Guard against uninteresting edge cases
    if (resourcePath == null || !resourcePath.contains('.')) {
      return resourcePath ?? '';
    }
    return resourcePath.substring(0, resourcePath.lastIndexOf('.'));
  }
}
