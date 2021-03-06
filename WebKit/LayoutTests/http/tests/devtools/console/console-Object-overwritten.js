// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(
      `Tests that Web Inspector's console is not broken if Object is overwritten in the inspected page. Test passes if the expression is evaluated in the console and no errors printed. Bug 101320.\n`);

  await TestRunner.loadModule('console_test_runner');
  await TestRunner.showPanel('console');

  await TestRunner.loadHTML(`
    <p>
    Tests that Web Inspector's console is not broken if Object is overwritten in the inspected page.
    Test passes if the expression is evaluated in the console and no errors printed.
    <a href="https://bugs.webkit.org/show_bug.cgi?id=101320">Bug 101320.</a>
    </p>
  `);

  await TestRunner.evaluateInPagePromise(`
    Object = function() {};
  `);

  ConsoleTestRunner.evaluateInConsole('var foo = {bar:2012}; foo', step1);

  function step1() {
    ConsoleTestRunner.dumpConsoleMessages();
    TestRunner.completeTest();
  }
})();
