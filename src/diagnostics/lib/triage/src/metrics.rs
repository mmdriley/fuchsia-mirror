// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod fetch;

use {
    super::config::{self},
    fetch::{InspectFetcher, SelectorString, SelectorType},
    fuchsia_inspect_node_hierarchy::Property as DiagnosticProperty,
    serde::{Deserialize, Deserializer},
    serde_json::Value as JsonValue,
    std::{clone::Clone, collections::HashMap, convert::TryFrom},
};

/// The contents of a single Metric. Metrics produce a value for use in Actions or other Metrics.
#[derive(Clone, Debug)]
pub enum Metric {
    /// Selector tells where to find a value in the Inspect data.
    // Note: This can't be a fidl_fuchsia_diagnostics::Selector because it's not deserializable or
    // cloneable.
    Selector(SelectorString),
    /// Eval contains an arithmetic expression,
    // TODO(cphoenix): Parse and validate this at load-time.
    Eval(String),
}

impl<'de> Deserialize<'de> for Metric {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        if SelectorString::is_selector(&value) {
            Ok(Metric::Selector(SelectorString::try_from(value).map_err(serde::de::Error::custom)?))
        } else {
            Ok(Metric::Eval(value))
        }
    }
}

impl std::fmt::Display for Metric {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Metric::Selector(s) => write!(f, "{:?}", s),
            Metric::Eval(s) => write!(f, "{}", s),
        }
    }
}

/// [Metrics] are a map from namespaces to the named [Metric]s stored within that namespace.
pub type Metrics = HashMap<String, HashMap<String, Metric>>;

/// Contains all the information needed to look up and evaluate a Metric - other
/// [Metric]s that may be referred to, and a source of input values to calculate on.
pub struct MetricState<'a> {
    pub metrics: &'a Metrics,
    pub fetcher: Fetcher<'a>,
}

/// [Fetcher] is a source of values to feed into the calculations. It may contain data either
/// from bugreport.zip files (e.g. inspect.json data that can be accessed via "select" entries)
/// or supplied in the specification of a trial.
pub enum Fetcher<'a> {
    FileData(FileDataFetcher<'a>),
    TrialData(TrialDataFetcher<'a>),
}

/// [FileDataFetcher] contains fetchers for data in bugreport.zip files.
#[derive(Clone)]
pub struct FileDataFetcher<'a> {
    inspect: &'a InspectFetcher,
}

impl<'a> FileDataFetcher<'a> {
    pub fn new(inspect: &'a InspectFetcher) -> FileDataFetcher<'a> {
        FileDataFetcher { inspect }
    }

    fn fetch(&self, selector: &SelectorString) -> MetricValue {
        match selector.selector_type {
            SelectorType::Inspect => {
                let values = self.inspect.fetch(&selector);
                match values.len() {
                    0 => MetricValue::Missing(format!(
                        "{} not found in Inspect data",
                        selector.body()
                    )),
                    1 => values[0].clone(),
                    _ => MetricValue::Missing(format!(
                        "Multiple {} found in Inspect data",
                        selector.body()
                    )),
                }
            }
        }
    }
}

/// [TrialDataFetcher] stores the key-value lookup for metric names whose values are given as
/// part of a trial (under the "test" section of the .triage files).
#[derive(Clone)]
pub struct TrialDataFetcher<'a> {
    values: &'a HashMap<String, JsonValue>,
}

impl<'a> TrialDataFetcher<'a> {
    pub fn new(values: &'a HashMap<String, JsonValue>) -> TrialDataFetcher<'a> {
        TrialDataFetcher { values }
    }

    fn fetch(&self, name: &str) -> MetricValue {
        match self.values.get(name) {
            Some(value) => MetricValue::from(value),
            None => MetricValue::Missing(format!("Value {} not overridden in test", name)),
        }
    }
}

/// The calculated or selected value of a Metric.
///
/// Missing means that the value could not be calculated; its String tells
/// the reason. Array and String are not used in v0.1 but will be useful later.
#[derive(Deserialize, Debug, Clone)]
pub enum MetricValue {
    // TODO(cphoenix): Support u64.
    Int(i64),
    Float(f64),
    String(String),
    Bool(bool),
    Array(Vec<MetricValue>),
    Bytes(Vec<u8>),
    Missing(String),
}

impl PartialEq for MetricValue {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (MetricValue::Int(l), MetricValue::Int(r)) => l == r,
            (MetricValue::Float(l), MetricValue::Float(r)) => l == r,
            (MetricValue::Int(l), MetricValue::Float(r)) => *l as f64 == *r,
            (MetricValue::Float(l), MetricValue::Int(r)) => *l == *r as f64,
            (MetricValue::String(l), MetricValue::String(r)) => l == r,
            (MetricValue::Bool(l), MetricValue::Bool(r)) => l == r,
            (MetricValue::Array(l), MetricValue::Array(r)) => l == r,
            (MetricValue::Missing(l), MetricValue::Missing(r)) => l == r,
            _ => false,
        }
    }
}

impl Eq for MetricValue {}

impl std::fmt::Display for MetricValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match &*self {
            MetricValue::Int(n) => write!(f, "Int({})", n),
            MetricValue::Float(n) => write!(f, "Float({})", n),
            MetricValue::Bool(n) => write!(f, "Bool({})", n),
            MetricValue::String(n) => write!(f, "String({})", n),
            MetricValue::Array(n) => write!(f, "Array({:?})", n),
            MetricValue::Bytes(n) => write!(f, "Bytes({:?})", n),
            MetricValue::Missing(n) => write!(f, "Missing({})", n),
        }
    }
}

impl Into<MetricValue> for f64 {
    fn into(self) -> MetricValue {
        MetricValue::Float(self)
    }
}

impl Into<MetricValue> for i64 {
    fn into(self) -> MetricValue {
        MetricValue::Int(self)
    }
}

impl From<DiagnosticProperty> for MetricValue {
    fn from(property: DiagnosticProperty) -> Self {
        match property {
            DiagnosticProperty::String(_name, value) => Self::String(value),
            DiagnosticProperty::Bytes(_name, value) => Self::Bytes(value),
            DiagnosticProperty::Int(_name, value) => Self::Int(value),
            DiagnosticProperty::Uint(_name, value) => Self::Int(value as i64),
            DiagnosticProperty::Double(_name, value) => Self::Float(value),
            DiagnosticProperty::Bool(_name, value) => Self::Bool(value),
            // TODO(cphoenix): Support arrays - need to figure out what to do about histograms.
            DiagnosticProperty::DoubleArray(_name, _)
            | DiagnosticProperty::IntArray(_name, _)
            | DiagnosticProperty::UintArray(_name, _) => {
                Self::Missing("Arrays not supported yet".to_owned())
            }
        }
    }
}

impl From<JsonValue> for MetricValue {
    fn from(value: JsonValue) -> Self {
        match value {
            JsonValue::String(value) => Self::String(value),
            JsonValue::Bool(value) => Self::Bool(value),
            JsonValue::Number(value) => {
                if value.is_i64() {
                    Self::Int(value.as_i64().unwrap())
                } else if value.is_f64() {
                    Self::Float(value.as_f64().unwrap())
                } else {
                    Self::Missing("Unable to convert JSON number".to_owned())
                }
            }
            _ => Self::Missing("Unsupported JSON type".to_owned()),
        }
    }
}

impl From<&JsonValue> for MetricValue {
    fn from(value: &JsonValue) -> Self {
        match value {
            JsonValue::String(value) => Self::String(value.clone()),
            JsonValue::Bool(value) => Self::Bool(*value),
            JsonValue::Number(value) => {
                if value.is_i64() {
                    Self::Int(value.as_i64().unwrap())
                } else if value.is_f64() {
                    Self::Float(value.as_f64().unwrap())
                } else {
                    Self::Missing("Unable to convert JSON number".to_owned())
                }
            }
            _ => Self::Missing("Unsupported JSON type".to_owned()),
        }
    }
}

#[derive(Deserialize, Debug, Clone, PartialEq)]
pub enum Function {
    Add,
    Sub,
    Mul,
    FloatDiv,
    IntDiv,
    Greater,
    Less,
    GreaterEq,
    LessEq,
    Equals,
    NotEq,
    Max,
    Min,
    And,
    Or,
    Not,
}

fn demand_numeric(value: &MetricValue) -> MetricValue {
    match value {
        MetricValue::Int(_) | MetricValue::Float(_) => {
            MetricValue::Missing("Internal bug - numeric passed to demand_numeric".to_string())
        }
        MetricValue::Missing(message) => MetricValue::Missing(message.clone()),
        other => MetricValue::Missing(format!("{} not numeric", other)),
    }
}

fn demand_both_numeric(value1: &MetricValue, value2: &MetricValue) -> MetricValue {
    match value1 {
        MetricValue::Float(_) | MetricValue::Int(_) => return demand_numeric(value2),
        _ => (),
    }
    match value2 {
        MetricValue::Float(_) | MetricValue::Int(_) => return demand_numeric(value1),
        _ => (),
    }
    let value1 = demand_numeric(value1);
    let value2 = demand_numeric(value2);
    MetricValue::Missing(format!("{} and {} not numeric", value1, value2))
}

/// Macro which handles applying a function to 2 operands and returns a
/// MetricValue.
///
/// The macro handles type promotion and promotion to the specified type.
macro_rules! apply_math_operands {
    ($left:expr, $right:expr, $function:expr, $ty:ty) => {
        match ($left, $right) {
            (MetricValue::Int(int1), MetricValue::Int(int2)) => {
                // TODO(cphoenix): Instead of converting to float, use int functions.
                ($function(int1 as f64, int2 as f64) as $ty).into()
            }
            (MetricValue::Int(int1), MetricValue::Float(float2)) => {
                $function(int1 as f64, float2).into()
            }
            (MetricValue::Float(float1), MetricValue::Int(int2)) => {
                $function(float1, int2 as f64).into()
            }
            (MetricValue::Float(float1), MetricValue::Float(float2)) => {
                $function(float1, float2).into()
            }
            (value1, value2) => demand_both_numeric(&value1, &value2),
        }
    };
}

/// A macro which extracts two binary operands from a vec of operands and
/// applies the given function.
macro_rules! extract_and_apply_math_operands {
    ($self:ident, $namespace:expr, $function:expr, $operands:expr, $ty:ty) => {
        match MetricState::extract_binary_operands($self, $namespace, $operands) {
            Ok((left, right)) => apply_math_operands!(left, right, $function, $ty),
            Err(value) => value,
        }
    };
}

/// Expression represents the parsed body of an Eval Metric. It applies
/// a function to sub-expressions, or stores a Missing error, the name of a
/// Metric, or a basic Value.
#[derive(Deserialize, Debug, Clone, PartialEq)]
pub enum Expression {
    // Some operators have arity 1 or 2, some have arity N.
    // For symmetry/readability, I use the same operand-spec Vec<Expression> for all.
    // TODO(cphoenix): Check on load that all operators have a legal number of operands.
    Function(Function, Vec<Expression>),
    IsMissing(Vec<Expression>),
    Metric(String),
    Value(MetricValue),
}

impl<'a> MetricState<'a> {
    /// Create an initialized MetricState.
    pub fn new(metrics: &'a Metrics, fetcher: Fetcher<'a>) -> MetricState<'a> {
        MetricState { metrics, fetcher }
    }

    /// Calculate the value of a Metric specified by name and namespace.
    ///
    /// If [name] is of the form "namespace::name" then [namespace] is ignored.
    /// If [name] is just "name" then [namespace] is used.
    fn metric_value_by_name(&self, namespace: &str, name: &String) -> MetricValue {
        // TODO(cphoenix): When historical metrics are added, change semantics to refresh()
        // TODO(cphoenix): cache values
        // TODO(cphoenix): Detect infinite cycles/depth.
        // TODO(cphoenix): Improve the data structure on Metric names. Probably fill in
        //  namespace during parse.
        let name_parts = name.split("::").collect::<Vec<_>>();
        let real_namespace: &str;
        let real_name: &str;
        match name_parts.len() {
            1 => {
                real_namespace = namespace;
                real_name = name;
            }
            2 => {
                real_namespace = name_parts[0];
                real_name = name_parts[1];
            }
            _ => {
                return MetricValue::Missing(format!("Bad name '{}': too many '::'", name));
            }
        }
        match self.metrics.get(real_namespace) {
            None => return MetricValue::Missing(format!("Bad namespace '{}'", real_namespace)),
            Some(metric_map) => match metric_map.get(real_name) {
                None => {
                    return MetricValue::Missing(format!(
                        "Metric '{}' Not Found in '{}'",
                        real_name, real_namespace
                    ))
                }
                Some(metric) => match metric {
                    Metric::Selector(selector) => match &self.fetcher {
                        Fetcher::FileData(fetcher) => fetcher.fetch(selector),
                        Fetcher::TrialData(fetcher) => fetcher.fetch(name),
                    },
                    Metric::Eval(expression) => self.eval_value(real_namespace, &expression),
                },
            },
        }
    }

    /// Fetch or compute the value of a Metric expression from an action.
    pub fn eval_action_metric(&self, namespace: &str, metric: &Metric) -> MetricValue {
        match metric {
            Metric::Selector(_) => {
                MetricValue::Missing("Selectors aren't allowed in action triggers".to_owned())
            }
            Metric::Eval(string) => self.eval_value(namespace, string),
        }
    }

    fn eval_value(&self, namespace: &str, expression: &str) -> MetricValue {
        match config::parse::parse_expression(expression) {
            Ok(expr) => self.evaluate(namespace, &expr),
            Err(e) => MetricValue::Missing(format!("Expression parse error\n{}", e)),
        }
    }

    /// Evaluate an Expression which contains only base values, not referring to other Metrics.
    #[cfg(test)]
    pub fn evaluate_math(e: &Expression) -> MetricValue {
        let map = HashMap::new();
        let fetcher = Fetcher::TrialData(TrialDataFetcher::new(&map));
        MetricState::new(&HashMap::new(), fetcher).evaluate(&"".to_string(), e)
    }

    fn evaluate_function(
        &self,
        namespace: &str,
        function: &Function,
        operands: &Vec<Expression>,
    ) -> MetricValue {
        match function {
            Function::Add => self.fold_math(namespace, &|a, b| a + b, operands),
            Function::Sub => self.apply_math(namespace, &|a, b| a - b, operands),
            Function::Mul => self.fold_math(namespace, &|a, b| a * b, operands),
            Function::FloatDiv => self.apply_math_f(namespace, &|a, b| a / b, operands),
            Function::IntDiv => self.apply_math(namespace, &|a, b| f64::trunc(a / b), operands),
            Function::Greater => self.apply_cmp(namespace, &|a, b| a > b, operands),
            Function::Less => self.apply_cmp(namespace, &|a, b| a < b, operands),
            Function::GreaterEq => self.apply_cmp(namespace, &|a, b| a >= b, operands),
            Function::LessEq => self.apply_cmp(namespace, &|a, b| a <= b, operands),
            Function::Equals => self.apply_metric_cmp(namespace, &|a, b| a == b, operands),
            Function::NotEq => self.apply_metric_cmp(namespace, &|a, b| a != b, operands),
            Function::Max => self.fold_math(namespace, &|a, b| if a > b { a } else { b }, operands),
            Function::Min => self.fold_math(namespace, &|a, b| if a < b { a } else { b }, operands),
            Function::And => self.fold_bool(namespace, &|a, b| a && b, operands),
            Function::Or => self.fold_bool(namespace, &|a, b| a || b, operands),
            Function::Not => self.not_bool(namespace, operands),
        }
    }

    fn evaluate(&self, namespace: &str, e: &Expression) -> MetricValue {
        match e {
            Expression::Function(f, operands) => self.evaluate_function(namespace, f, operands),
            Expression::IsMissing(operands) => self.is_missing(namespace, operands),
            Expression::Metric(name) => self.metric_value_by_name(namespace, name),
            Expression::Value(value) => value.clone(),
        }
    }

    // Applies an operator (which should be associative and commutative) to a list of operands.
    fn fold_math(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() == 0 {
            return MetricValue::Missing("No operands in math expression".into());
        }
        let mut result: MetricValue = self.evaluate(namespace, &operands[0]);
        for operand in operands[1..].iter() {
            result = self.apply_math(
                namespace,
                function,
                &vec![Expression::Value(result), operand.clone()],
            );
        }
        result
    }

    // Applies a given function to two values, handling type-promotion.
    // This function will return a MetricValue::Int if both values are ints
    // and a MetricValue::Float if not.
    fn apply_math(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        extract_and_apply_math_operands!(self, namespace, function, operands, i64)
    }

    // Applies a given function to two values, handling type-promotion.
    // This function will always return a MetricValue::Float
    fn apply_math_f(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        extract_and_apply_math_operands!(self, namespace, function, operands, f64)
    }

    fn extract_binary_operands(
        &self,
        namespace: &str,
        operands: &Vec<Expression>,
    ) -> Result<(MetricValue, MetricValue), MetricValue> {
        if operands.len() != 2 {
            return Err(MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            )));
        }
        Ok((self.evaluate(namespace, &operands[0]), self.evaluate(namespace, &operands[1])))
    }

    // Applies an ord operator to two numbers. (>, >=, <, <=)
    fn apply_cmp(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> bool),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() != 2 {
            return MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            ));
        }
        let result = match (
            self.evaluate(namespace, &operands[0]),
            self.evaluate(namespace, &operands[1]),
        ) {
            // TODO(cphoenix): Instead of converting two ints to float, use int functions.
            (MetricValue::Int(int1), MetricValue::Int(int2)) => function(int1 as f64, int2 as f64),
            (MetricValue::Int(int1), MetricValue::Float(float2)) => function(int1 as f64, float2),
            (MetricValue::Float(float1), MetricValue::Int(int2)) => function(float1, int2 as f64),
            (MetricValue::Float(float1), MetricValue::Float(float2)) => function(float1, float2),
            (value1, value2) => return demand_both_numeric(&value1, &value2),
        };
        MetricValue::Bool(result)
    }

    // Transitional Function to allow for string equality comparisons.
    // This function will eventually replace the apply_cmp function once MetricValue
    // implements the std::cmp::PartialOrd trait
    fn apply_metric_cmp(
        &self,
        namespace: &str,
        function: &dyn (Fn(&MetricValue, &MetricValue) -> bool),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() != 2 {
            return MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            ));
        }
        let left = self.evaluate(namespace, &operands[0]);
        let right = self.evaluate(namespace, &operands[1]);

        match (&left, &right) {
            // We forward ::Missing for better error messaging.
            (MetricValue::Missing(reason), _) => MetricValue::Missing(reason.to_string()),
            (_, MetricValue::Missing(reason)) => MetricValue::Missing(reason.to_string()),
            _ => MetricValue::Bool(function(&left, &right)),
        }
    }

    fn fold_bool(
        &self,
        namespace: &str,
        function: &dyn (Fn(bool, bool) -> bool),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() == 0 {
            return MetricValue::Missing("No operands in boolean expression".into());
        }
        let mut result: bool = match self.evaluate(namespace, &operands[0]) {
            MetricValue::Bool(value) => value,
            MetricValue::Missing(reason) => {
                return MetricValue::Missing(reason);
            }
            bad => return MetricValue::Missing(format!("{:?} is not boolean", bad)),
        };
        for operand in operands[1..].iter() {
            result = match self.evaluate(namespace, operand) {
                MetricValue::Bool(value) => function(result, value),
                MetricValue::Missing(reason) => {
                    return MetricValue::Missing(reason);
                }
                bad => return MetricValue::Missing(format!("{:?} is not boolean", bad)),
            }
        }
        MetricValue::Bool(result)
    }

    fn not_bool(&self, namespace: &str, operands: &Vec<Expression>) -> MetricValue {
        if operands.len() != 1 {
            return MetricValue::Missing(format!(
                "Wrong number of args ({}) for unary bool operator",
                operands.len()
            ));
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::Bool(true) => MetricValue::Bool(false),
            MetricValue::Bool(false) => MetricValue::Bool(true),
            MetricValue::Missing(reason) => {
                return MetricValue::Missing(reason);
            }
            bad => return MetricValue::Missing(format!("{:?} not boolean", bad)),
        }
    }

    // Returns Bool true if the given metric is Missing, false if the metric has a value.
    fn is_missing(&self, namespace: &str, operands: &Vec<Expression>) -> MetricValue {
        if operands.len() != 1 {
            return MetricValue::Missing(format!("Bad operand"));
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::Missing(_) => MetricValue::Bool(true),
            _ => MetricValue::Bool(false),
        }
    }
}

// The evaluation of math expressions is tested pretty exhaustively in parse.rs unit tests.

// The use of metric names in expressions and actions, with and without namespaces, is tested in
// the integration test.
//   $ fx test triage_lib_test

#[cfg(test)]
mod test {
    use super::*;
    use lazy_static::lazy_static;

    #[test]
    fn test_equality() {
        // Equal Value, Equal Type
        assert_eq!(MetricValue::Int(1), MetricValue::Int(1));
        assert_eq!(MetricValue::Float(1.0), MetricValue::Float(1.0));
        assert_eq!(MetricValue::String("A".to_string()), MetricValue::String("A".to_string()));
        assert_eq!(MetricValue::Bool(true), MetricValue::Bool(true));
        assert_eq!(MetricValue::Bool(false), MetricValue::Bool(false));
        assert_eq!(
            MetricValue::Array(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Array(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ])
        );

        assert_eq!(MetricValue::Int(1), MetricValue::Float(1.0));

        // Nested array
        assert_eq!(
            MetricValue::Array(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Array(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ])
        );

        // Missing should never be equal
        assert_eq!(
            MetricValue::Missing("err".to_string()),
            MetricValue::Missing("err".to_string())
        );
    }

    #[test]
    fn test_inequality() {
        // Different Value, Equal Type
        assert_ne!(MetricValue::Int(1), MetricValue::Int(2));
        assert_ne!(MetricValue::Float(1.0), MetricValue::Float(2.0));
        assert_ne!(MetricValue::String("A".to_string()), MetricValue::String("B".to_string()));
        assert_ne!(MetricValue::Bool(true), MetricValue::Bool(false));
        assert_ne!(
            MetricValue::Array(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Array(vec![
                MetricValue::Int(2),
                MetricValue::Float(2.0),
                MetricValue::String("B".to_string()),
                MetricValue::Bool(false),
            ])
        );

        // Different Type
        assert_ne!(MetricValue::Int(2), MetricValue::Float(1.0));
        assert_ne!(MetricValue::Int(1), MetricValue::String("A".to_string()));
        assert_ne!(MetricValue::Int(1), MetricValue::Bool(true));
        assert_ne!(MetricValue::Float(1.0), MetricValue::String("A".to_string()));
        assert_ne!(MetricValue::Float(1.0), MetricValue::Bool(true));
        assert_ne!(MetricValue::String("A".to_string()), MetricValue::Bool(true));
    }

    #[test]
    fn test_fmt() {
        assert_eq!(format!("{}", MetricValue::Int(3)), "Int(3)");
        assert_eq!(format!("{}", MetricValue::Float(3.5)), "Float(3.5)");
        assert_eq!(format!("{}", MetricValue::Bool(true)), "Bool(true)");
        assert_eq!(format!("{}", MetricValue::Bool(false)), "Bool(false)");
        assert_eq!(format!("{}", MetricValue::String("cat".to_string())), "String(cat)");
        assert_eq!(
            format!("{}", MetricValue::Array(vec![MetricValue::Int(1), MetricValue::Float(2.5)])),
            "Array([Int(1), Float(2.5)])"
        );
        assert_eq!(format!("{}", MetricValue::Bytes(vec![1u8, 2u8])), "Bytes([1, 2])");
        assert_eq!(
            format!("{}", MetricValue::Missing("Where is Waldo?".to_string())),
            "Missing(Where is Waldo?)"
        );
    }

    lazy_static! {
        static ref LOCAL_M: HashMap<String, JsonValue> = {
            let mut m = HashMap::new();
            m.insert("foo".to_owned(), JsonValue::try_from(42).unwrap());
            m
        };
        static ref FOO_42_TRIAL_FETCHER: TrialDataFetcher<'static> =
            TrialDataFetcher::new(&LOCAL_M);
        static ref LOCAL_F: InspectFetcher = {
            let s = r#"[{
                "data_source": "Inspect",
                "moniker": "bar.cmx",
                "payload": { "root": { "bar": 99 }}
            }]"#;
            InspectFetcher::try_from(&*s.to_owned()).unwrap()
        };
        static ref BAR_99_FILE_FETCHER: FileDataFetcher<'static> = FileDataFetcher::new(&LOCAL_F);
        static ref BAR_SELECTOR: SelectorString =
            SelectorString::try_from("INSPECT:bar.cmx:root:bar".to_owned()).unwrap();
        static ref WRONG_SELECTOR: SelectorString =
            SelectorString::try_from("INSPECT:bar.cmx:root:oops".to_owned()).unwrap();
    }

    fn assert_missing(value: MetricValue, message: &'static str) {
        match value {
            MetricValue::Missing(_) => {}
            _ => assert!(false, message),
        }
    }

    #[test]
    fn test_file_fetch() {
        assert_eq!(BAR_99_FILE_FETCHER.fetch(&BAR_SELECTOR), MetricValue::Int(99));
        assert_missing(
            BAR_99_FILE_FETCHER.fetch(&WRONG_SELECTOR),
            "File fetcher found bogus selector",
        );
    }

    #[test]
    fn test_trial_fetch() {
        assert_eq!(FOO_42_TRIAL_FETCHER.fetch("foo"), MetricValue::Int(42));
        assert_missing(FOO_42_TRIAL_FETCHER.fetch("oops"), "Trial fetcher found bogus selector");
    }

    #[test]
    fn test_eval_with_file() {
        let mut file_map = HashMap::new();
        file_map.insert("bar".to_owned(), Metric::Selector(BAR_SELECTOR.clone()));
        file_map.insert("bar_plus_one".to_owned(), Metric::Eval("bar+1".to_owned()));
        file_map.insert("oops_plus_one".to_owned(), Metric::Eval("oops+1".to_owned()));
        let mut metrics = HashMap::new();
        metrics.insert("bar_file".to_owned(), file_map);
        let file_state = MetricState::new(&metrics, Fetcher::FileData(BAR_99_FILE_FETCHER.clone()));
        assert_eq!(
            file_state.metric_value_by_name("bar_file", &"bar_plus_one".to_owned()),
            MetricValue::Int(100)
        );
        assert_missing(
            file_state.metric_value_by_name("bar_file", &"oops_plus_one".to_owned()),
            "File found nonexistent name",
        );
    }

    #[test]
    fn test_eval_with_trial() {
        let mut trial_map = HashMap::new();
        // The (broken) "foo" selector should be ignored in favor of the "foo" fetched value.
        trial_map.insert("foo".to_owned(), Metric::Selector(BAR_SELECTOR.clone()));
        trial_map.insert("foo_plus_one".to_owned(), Metric::Eval("foo+1".to_owned()));
        trial_map.insert("oops_plus_one".to_owned(), Metric::Eval("oops+1".to_owned()));
        let mut metrics = HashMap::new();
        metrics.insert("foo_file".to_owned(), trial_map);
        let trial_state =
            MetricState::new(&metrics, Fetcher::TrialData(FOO_42_TRIAL_FETCHER.clone()));
        assert_eq!(
            trial_state.metric_value_by_name("foo_file", &"foo_plus_one".to_owned()),
            MetricValue::Int(43)
        );
        assert_missing(
            trial_state.metric_value_by_name("foo_file", &"oops_plus_one".to_owned()),
            "Trial found nonexistent name",
        );
    }
}
