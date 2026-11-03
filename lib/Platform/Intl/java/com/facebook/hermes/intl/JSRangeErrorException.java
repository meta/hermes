/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.hermes.intl;

// If an Intl method throws this, it will be converted to a RangeError
// exception in JS.
@SuppressWarnings("serial") // no cross-version serialization; UID left computed
public class JSRangeErrorException extends Exception {
  private static final long serialVersionUID = 1L;

  public JSRangeErrorException() {}

  public JSRangeErrorException(String message) {
    super(message);
  }

  public JSRangeErrorException(String message, Throwable cause) {
    super(message, cause);
  }

  public JSRangeErrorException(Throwable cause) {
    super(cause);
  }
}
