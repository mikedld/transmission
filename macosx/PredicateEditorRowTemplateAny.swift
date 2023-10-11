// This file Copyright © 2009-2023 Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

import AppKit

class PredicateEditorRowTemplateAny: NSPredicateEditorRowTemplate {
    override func predicate(withSubpredicates subpredicates: [NSPredicate]?) -> NSPredicate {
        // we only make NSComparisonPredicates
        // swiftlint:disable:next force_cast
        let predicate = super.predicate(withSubpredicates: subpredicates) as! NSComparisonPredicate

        // construct a near-identical predicate
        return NSComparisonPredicate(leftExpression: predicate.leftExpression,
                                     rightExpression: predicate.rightExpression,
                                     modifier: .any,
                                     type: predicate.predicateOperatorType,
                                     options: predicate.options)
    }
}
