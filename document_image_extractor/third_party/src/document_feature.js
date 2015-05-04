goog.provide('image.collections.extension.DocumentFeature');

goog.scope(function() {



/**
 * This is a base class for all document features (e.g. title, snippet, image).
 * @param {number} relevance Relevance of this feature to the document.
 * @constructor
 */
image.collections.extension.DocumentFeature = function(relevance) {
  /** @private {number} */
  this.relevance_ = relevance;
};
var DocumentFeature = image.collections.extension.DocumentFeature;


/**
 * Returns the feature relevance.
 * @return {number}
 */
DocumentFeature.prototype.getRelevance = function() {
  return this.relevance_;
};


/**
 * Compares two document features by their relevance.
 * @param {!DocumentFeature} feature1
 * @param {!DocumentFeature} feature2
 * @return {number}
 */
DocumentFeature.compare = function(feature1, feature2) {
  if (feature1 == feature2) {
    return 0;
  }
  return feature1.getRelevance() - feature2.getRelevance();
};
});  // goog.scope
