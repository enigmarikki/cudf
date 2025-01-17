/*
 *
 *  Copyright (c) 2023, NVIDIA CORPORATION.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

package ai.rapids.cudf;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class CSVWriterOptions {

  private String[] columnNames;
  private Boolean includeHeader = false;
  private String rowDelimiter = "\n";
  private byte fieldDelimiter = ',';
  private String nullValue = "";
  private String falseValue = "false";
  private String trueValue = "true";

  private CSVWriterOptions(Builder builder) {
    this.columnNames = builder.columnNames.toArray(new String[builder.columnNames.size()]);
    this.nullValue = builder.nullValue;
    this.includeHeader = builder.includeHeader;
    this.fieldDelimiter = builder.fieldDelimiter;
    this.rowDelimiter = builder.rowDelimiter;
    this.falseValue = builder.falseValue;
    this.trueValue = builder.trueValue;
  }

  public String[] getColumnNames() {
    return columnNames;
  }

  public Boolean getIncludeHeader() {
    return includeHeader;
  }

  public String getRowDelimiter() {
    return rowDelimiter;
  }

  public byte getFieldDelimiter() {
    return fieldDelimiter;
  }

  public String getNullValue() {
    return nullValue;
  }

  public String getTrueValue() {
    return trueValue;
  }

  public String getFalseValue() {
    return falseValue;
  }

  public static Builder builder() {
    return new Builder();
  }

  public static class Builder {

    private List<String> columnNames = Collections.emptyList();
    private Boolean includeHeader = false;
    private String rowDelimiter = "\n";
    private byte fieldDelimiter = ',';
    private String nullValue = "";
    private String falseValue = "false";
    private String trueValue = "true";

    public CSVWriterOptions build() {
      return new CSVWriterOptions(this);
    }

    public Builder withColumnNames(List<String> columnNames) {
      this.columnNames = columnNames;
      return this;
    }

    public Builder withColumnNames(String... columnNames) {
      List<String> columnNamesList = new ArrayList<>();
      for (String columnName : columnNames) {
        columnNamesList.add(columnName);
      }
      return withColumnNames(columnNamesList);
    }

    public Builder withIncludeHeader(Boolean includeHeader) {
      this.includeHeader = includeHeader;
      return this;
    }

    public Builder withRowDelimiter(String rowDelimiter) {
      this.rowDelimiter = rowDelimiter;
      return this;
    }

    public Builder withFieldDelimiter(byte fieldDelimiter) {
      this.fieldDelimiter = fieldDelimiter;
      return this;
    }

    public Builder withNullValue(String nullValue) {
      this.nullValue = nullValue;
      return this;
    }

    public Builder withTrueValue(String trueValue) {
      this.trueValue = trueValue;
      return this;
    }

    public Builder withFalseValue(String falseValue) {
      this.falseValue = falseValue;
      return this;
    }
  }
}
